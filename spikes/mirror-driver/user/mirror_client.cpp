#include "mirror_client.h"

#include <dwmapi.h>

#include <cstring>
#include <cstdio>

// Diagnostic sink: printf (host-agent captures stderr; mirror_probe prints to
// console). media_log.h is not visible to the standalone mirror_probe build.
#define RDM_DIAG(fmt, ...) std::printf("[mirror_client] " fmt "\n", ##__VA_ARGS__)

namespace {
// Last attach failure code (DISP_CHANGE_*) — readable by host via getter.
long g_last_attach_error = 0;


const char* kDriverString = ROAD_DESK_MIRROR_DISPLAY_NAME;
const char* kMiniportService = "rdmmini";

bool device_string_is_ours(const char* device_string) {
  if (!device_string || !device_string[0]) {
    return false;
  }
  if (std::strcmp(device_string, kDriverString) == 0) {
    return true;
  }
  return std::strstr(device_string, "Road Desk Mirror") != nullptr;
}

bool get_primary_size(DWORD* cx, DWORD* cy) {
  DEVMODEA dm{};
  dm.dmSize = sizeof(dm);
  DISPLAY_DEVICEA dd{};
  dd.cb = sizeof(dd);
  for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i) {
    // Never take size from a mirror — Road Desk can wrongly show PRIMARY|MIRROR.
    if (dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) {
      continue;
    }
    if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
      if (!EnumDisplaySettingsA(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
        return false;
      }
      *cx = dm.dmPelsWidth;
      *cy = dm.dmPelsHeight;
      return true;
    }
  }
  return false;
}

bool find_mirror_device_ex(char* out_name, size_t out_len, char* out_key, size_t key_len,
                           DWORD* out_flags) {
  if (!out_name || out_len < 16) {
    return false;
  }
  DISPLAY_DEVICEA dd{};
  dd.cb = sizeof(dd);
  for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i) {
    if (!device_string_is_ours(dd.DeviceString)) {
      continue;
    }
    std::strncpy(out_name, dd.DeviceName, out_len - 1);
    out_name[out_len - 1] = '\0';
    if (out_key && key_len) {
      std::strncpy(out_key, dd.DeviceKey, key_len - 1);
      out_key[key_len - 1] = '\0';
    }
    if (out_flags) {
      *out_flags = dd.StateFlags;
    }
    return true;
  }
  return false;
}

// Clear Attach.ToDesktop under a HKLM path (relative, no leading machine prefix).
bool reg_set_attach(const char* subkey, DWORD value) {
  HKEY h = nullptr;
  if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h,
                      nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const LONG rc =
      RegSetValueExA(h, "Attach.ToDesktop", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                     sizeof(value));
  RegCloseKey(h);
  return rc == ERROR_SUCCESS;
}

void write_attach_registry(const char* device_key, DWORD value) {
  // UltraVNC-style hardware-profile service keys.
  char base[256];
  std::snprintf(base, sizeof(base),
                "SYSTEM\\CurrentControlSet\\Hardware Profiles\\Current\\System\\"
                "CurrentControlSet\\Services\\%s",
                kMiniportService);
  static const char* kDevs[] = {"DEVICE0", "Device0", "DEVICE1", "Device1"};
  for (const char* d : kDevs) {
    char path[320];
    std::snprintf(path, sizeof(path), "%s\\%s", base, d);
    reg_set_attach(path, value);
  }
  std::snprintf(base, sizeof(base), "SYSTEM\\CurrentControlSet\\Services\\%s", kMiniportService);
  for (const char* d : kDevs) {
    char path[320];
    std::snprintf(path, sizeof(path), "%s\\%s", base, d);
    reg_set_attach(path, value);
  }
  if (device_key && device_key[0]) {
    const char* p = device_key;
    if (_strnicmp(p, "\\Registry\\Machine\\", 18) == 0) {
      p += 18;
    } else if (_strnicmp(p, "\\REGISTRY\\MACHINE\\", 18) == 0) {
      p += 18;
    }
    if (p[0]) {
      reg_set_attach(p, value);
    }
  }
}

void clear_attach_registry(const char* device_key) {
  write_attach_registry(device_key, 0);
}

// If Attach.ToDesktop exists and is non-zero, force it to 0 (do not create new values).
void zero_attach_if_set(HKEY h) {
  DWORD cur = 0;
  DWORD cb = sizeof(cur);
  DWORD type = 0;
  if (RegQueryValueExA(h, "Attach.ToDesktop", nullptr, &type, reinterpret_cast<LPBYTE>(&cur),
                       &cb) != ERROR_SUCCESS ||
      type != REG_DWORD || cur == 0) {
    return;
  }
  cur = 0;
  RegSetValueExA(h, "Attach.ToDesktop", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&cur),
                 sizeof(cur));
}

void scrub_attach_under_key(HKEY root, bool skip_rdmmini) {
  char name[256];
  for (DWORD i = 0; RegEnumKeyA(root, i, name, sizeof(name)) == ERROR_SUCCESS; ++i) {
    if (skip_rdmmini && _stricmp(name, kMiniportService) == 0) {
      continue;
    }
    HKEY svc = nullptr;
    if (RegOpenKeyExA(root, name, 0, KEY_READ | KEY_ENUMERATE_SUB_KEYS, &svc) != ERROR_SUCCESS) {
      continue;
    }
    static const char* kDevs[] = {"DEVICE0", "Device0", "DEVICE1", "Device1", "DEVICE2",
                                  "Device2"};
    for (const char* d : kDevs) {
      HKEY dev = nullptr;
      if (RegOpenKeyExA(svc, d, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &dev) == ERROR_SUCCESS) {
        zero_attach_if_set(dev);
        RegCloseKey(dev);
      }
    }
    {
      HKEY write = nullptr;
      if (RegOpenKeyExA(root, name, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &write) == ERROR_SUCCESS) {
        zero_attach_if_set(write);
        RegCloseKey(write);
      }
    }
    RegCloseKey(svc);
  }
}

void scrub_mirror_device_keys(bool skip_ours) {
  DISPLAY_DEVICEA dd{};
  dd.cb = sizeof(dd);
  for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i) {
    if (!(dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)) {
      continue;
    }
    if (skip_ours && device_string_is_ours(dd.DeviceString)) {
      continue;
    }
    if (!dd.DeviceKey[0]) {
      continue;
    }
    const char* p = dd.DeviceKey;
    if (_strnicmp(p, "\\Registry\\Machine\\", 18) == 0) {
      p += 18;
    } else if (_strnicmp(p, "\\REGISTRY\\MACHINE\\", 18) == 0) {
      p += 18;
    }
    HKEY h = nullptr;
    if (p[0] &&
        RegOpenKeyExA(HKEY_LOCAL_MACHINE, p, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &h) ==
            ERROR_SUCCESS) {
      zero_attach_if_set(h);
      RegCloseKey(h);
    }
  }
}

void scrub_service_trees(bool skip_rdmmini) {
  static const char* kRoots[] = {
      "SYSTEM\\CurrentControlSet\\Hardware Profiles\\Current\\System\\CurrentControlSet\\Services",
      "SYSTEM\\CurrentControlSet\\Services",
  };
  for (const char* root_path : kRoots) {
    HKEY root = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, root_path, 0, KEY_READ | KEY_ENUMERATE_SUB_KEYS,
                      &root) == ERROR_SUCCESS) {
      scrub_attach_under_key(root, skip_rdmmini);
      RegCloseKey(root);
    }
  }
}

void scrub_foreign_attach_registry() {
  // Do NOT touch rdmmini / our DeviceKey while attached — writing those during
  // Device Manager PnP refresh wedges mouse/keyboard on Win7.
  scrub_mirror_device_keys(/*skip_ours=*/true);
  scrub_service_trees(/*skip_rdmmini=*/true);
}

void scrub_all_attach_registry() {
  clear_attach_registry(nullptr);
  scrub_mirror_device_keys(/*skip_ours=*/false);
  scrub_service_trees(/*skip_rdmmini=*/false);
}

// CDS attach/detach can flip Win7 color scheme (Basic balloon). Preserve composition.
struct CompositionSnapshot {
  BOOL enabled = FALSE;
  bool have = false;
};

CompositionSnapshot snapshot_composition() {
  CompositionSnapshot s;
  if (SUCCEEDED(DwmIsCompositionEnabled(&s.enabled))) {
    s.have = true;
  }
  return s;
}

void try_restore_dwm_composition(const CompositionSnapshot* prefer = nullptr) {
  // If we knew composition was on (or unknown), nudge it back — do not force off
  // (forcing off is itself a theme change).
  const bool want_on = !prefer || !prefer->have || prefer->enabled;
  if (!want_on) {
    return;
  }
#pragma warning(push)
#pragma warning(disable : 4995)
  DwmEnableComposition(1);
#pragma warning(pop)
}

bool cds_detach_device(const char* device_name) {
  const CompositionSnapshot before = snapshot_composition();
  DEVMODEA dm{};
  dm.dmSize = sizeof(dm);
  dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION | DM_BITSPERPEL;
  dm.dmPelsWidth = 0;
  dm.dmPelsHeight = 0;
  dm.dmBitsPerPel = 32;
  // Prefer no UPDATEREGISTRY so detach does not re-stick Attach.ToDesktop=1 elsewhere.
  LONG code = ChangeDisplaySettingsExA(device_name, &dm, nullptr, CDS_NORESET, nullptr);
  if (code != DISP_CHANGE_SUCCESSFUL) {
    code = ChangeDisplaySettingsExA(device_name, &dm, nullptr,
                                    CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
  }
  if (code != DISP_CHANGE_SUCCESSFUL) {
    return false;
  }
  code = ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, 0, nullptr);
  try_restore_dwm_composition(&before);
  return code == DISP_CHANGE_SUCCESSFUL || code == DISP_CHANGE_RESTART;
}

}  // namespace

void rdm_dump_display_devices() {
  DISPLAY_DEVICEA dd{};
  dd.cb = sizeof(dd);
  std::printf("[mirror_client] EnumDisplayDevices:\n");
  DWORD found = 0;
  for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i) {
    ++found;
    std::printf("  [%lu] name=%s string=\"%s\" flags=0x%lx%s%s\n", i, dd.DeviceName,
                dd.DeviceString, dd.StateFlags,
                (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " PRIMARY" : "",
                (dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) ? " MIRROR" : "");
  }
  if (found == 0) {
    std::printf("  (none)\n");
  }
}

bool rdm_find_mirror_device(char* out_name, size_t out_len) {
  return find_mirror_device_ex(out_name, out_len, nullptr, 0, nullptr);
}

long rdm_last_attach_error() {
  return g_last_attach_error;
}

void rdm_scrub_foreign_attach_registry() {
  scrub_foreign_attach_registry();
}

void rdm_scrub_all_attach_registry() {
  scrub_all_attach_registry();
}

bool rdm_force_detach() {
  char name[128] = {};
  char key[512] = {};
  DWORD flags = 0;
  // Always scrub peers (VNC/Radmin) — Device Manager re-attaches anyone with Attach=1.
  scrub_all_attach_registry();
  if (!find_mirror_device_ex(name, sizeof(name), key, sizeof(key), &flags)) {
    return true;
  }
  clear_attach_registry(key);
  const bool ok = cds_detach_device(name);
  if (!ok) {
    std::printf("[mirror_client] CDS detach code failed device=%s flags=0x%lx (registry cleared)\n",
                name, flags);
  }
  scrub_all_attach_registry();
  return true;
}

bool rdm_attach_mirror(char* device_name, size_t device_name_len) {
  DWORD cx = 0;
  DWORD cy = 0;
  if (!get_primary_size(&cx, &cy) || cx == 0 || cy == 0) {
    RDM_DIAG("FAIL get primary size");
    SetLastError(ERROR_PROC_NOT_FOUND);
    rdm_dump_display_devices();
    return false;
  }
  char name[128] = {};
  char key[512] = {};
  if (!find_mirror_device_ex(name, sizeof(name), key, sizeof(key), nullptr)) {
    RDM_DIAG("FAIL find \"%s\" - reboot after INF install?", kDriverString);
    SetLastError(ERROR_DEVICE_NOT_CONNECTED);
    rdm_dump_display_devices();
    return false;
  }
  if (device_name && device_name_len) {
    std::strncpy(device_name, name, device_name_len - 1);
    device_name[device_name_len - 1] = '\0';
  }

  // Peers with Attach.ToDesktop=1 (VNC/Radmin) + Device Manager PnP = mouse freeze.
  scrub_foreign_attach_registry();

  const CompositionSnapshot before = snapshot_composition();

  // UltraVNC order: Attach.ToDesktop=1 in registry, then ChangeDisplaySettingsEx.
  // Prefer CDS_NORESET without UPDATEREGISTRY so attach is not persisted for PnP.
  write_attach_registry(key, 1);

  DEVMODEA dm{};
  dm.dmSize = sizeof(dm);
  dm.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;
  dm.dmBitsPerPel = 32;
  dm.dmPelsWidth = cx;
  dm.dmPelsHeight = cy;
  dm.dmPosition.x = 0;
  dm.dmPosition.y = 0;

  LONG code = ChangeDisplaySettingsExA(name, &dm, nullptr, CDS_NORESET, nullptr);
  if (code != DISP_CHANGE_SUCCESSFUL) {
    // Fallback: some Win7 stacks only attach with UPDATEREGISTRY.
    code = ChangeDisplaySettingsExA(name, &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET,
                                    nullptr);
  }
  if (code != DISP_CHANGE_SUCCESSFUL) {
    RDM_DIAG("CDS attach failed code=%ld device=%s %ux%u", code, name, cx, cy);
    g_last_attach_error = static_cast<long>(code);
    clear_attach_registry(key);
    scrub_foreign_attach_registry();
    return false;
  }
  code = ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, 0, nullptr);
  if (code != DISP_CHANGE_SUCCESSFUL && code != DISP_CHANGE_RESTART) {
    RDM_DIAG("CDS apply failed code=%ld", code);
    g_last_attach_error = static_cast<long>(code);
    clear_attach_registry(key);
    cds_detach_device(name);
    scrub_foreign_attach_registry();
    return false;
  }
  // Live-attached; registry must stay 0 for us, and peers scrubbed. Do not keep
  // pounding our DeviceKey every tick — that + Device Manager PnP freezes input.
  clear_attach_registry(key);
  scrub_foreign_attach_registry();
  try_restore_dwm_composition(&before);
  if (code == DISP_CHANGE_RESTART) {
    std::printf("[mirror_client] CDS asks for reboot - reboot then retry gm2\n");
  }
  return true;
}

bool rdm_detach_mirror(const char* device_name) {
  char name[128] = {};
  char key[512] = {};
  const bool found = find_mirror_device_ex(name, sizeof(name), key, sizeof(key), nullptr);
  scrub_all_attach_registry();
  const char* cds_name = (device_name && device_name[0]) ? device_name : (found ? name : nullptr);
  if (!cds_name) {
    return true;
  }
  const bool ok = cds_detach_device(cds_name);
  scrub_all_attach_registry();
  (void)key;
  return ok;
}

HDC rdm_create_mirror_dc(const char* device_name) {
  if (!device_name) {
    return nullptr;
  }
  return CreateDCA("DISPLAY", device_name, nullptr, nullptr);
}

bool rdm_escape_get_info(HDC hdc, RdmInfo* out) {
  if (!hdc || !out) {
    return false;
  }
  RdmInfo inf{};
  const int n = ExtEscape(hdc, static_cast<int>(RDM_ESC_GET_INFO), 0, nullptr,
                          sizeof(inf), reinterpret_cast<LPSTR>(&inf));
  if (n < static_cast<int>(sizeof(inf))) {
    return false;
  }
  *out = inf;
  return true;
}

uint32_t rdm_escape_get_dirty(HDC hdc, void* out_buf, uint32_t out_bytes) {
  if (!hdc || !out_buf || out_bytes < sizeof(RdmDirtyHeader)) {
    return 0;
  }
  const int n = ExtEscape(hdc, static_cast<int>(RDM_ESC_GET_DIRTY), 0, nullptr,
                          static_cast<int>(out_bytes),
                          reinterpret_cast<LPSTR>(out_buf));
  if (n <= 0) {
    return 0;
  }
  return static_cast<uint32_t>(n);
}

uint32_t rdm_escape_get_frame(HDC hdc, void* out_buf, uint32_t out_bytes) {
  if (!hdc || !out_buf || out_bytes < sizeof(RdmFrameHeader)) {
    return 0;
  }
  const int n = ExtEscape(hdc, static_cast<int>(RDM_ESC_GET_FRAME), 0, nullptr,
                          static_cast<int>(out_bytes),
                          reinterpret_cast<LPSTR>(out_buf));
  if (n <= 0) {
    return 0;
  }
  return static_cast<uint32_t>(n);
}
