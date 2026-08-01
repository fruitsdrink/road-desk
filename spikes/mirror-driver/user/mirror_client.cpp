#include "mirror_client.h"

#include <cstring>
#include <cstdio>

namespace {

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

bool cds_detach_device(const char* device_name) {
  DEVMODEA dm{};
  dm.dmSize = sizeof(dm);
  dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION | DM_BITSPERPEL;
  dm.dmPelsWidth = 0;
  dm.dmPelsHeight = 0;
  dm.dmBitsPerPel = 32;
  LONG code = ChangeDisplaySettingsExA(device_name, &dm, nullptr,
                                       CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
  if (code != DISP_CHANGE_SUCCESSFUL) {
    return false;
  }
  code = ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, 0, nullptr);
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

bool rdm_force_detach() {
  char name[128] = {};
  char key[512] = {};
  DWORD flags = 0;
  if (!find_mirror_device_ex(name, sizeof(name), key, sizeof(key), &flags)) {
    // Still try to clear known registry keys from sticky Attach.ToDesktop=1 installs.
    clear_attach_registry(nullptr);
    return true;
  }
  clear_attach_registry(key);
  const bool ok = cds_detach_device(name);
  if (!ok) {
    std::printf("[mirror_client] CDS detach code failed device=%s flags=0x%lx (registry cleared)\n",
                name, flags);
  }
  return true;
}

bool rdm_attach_mirror(char* device_name, size_t device_name_len) {
  DWORD cx = 0;
  DWORD cy = 0;
  if (!get_primary_size(&cx, &cy) || cx == 0 || cy == 0) {
    std::printf("[mirror_client] FAIL get primary size\n");
    rdm_dump_display_devices();
    return false;
  }
  char name[128] = {};
  char key[512] = {};
  if (!find_mirror_device_ex(name, sizeof(name), key, sizeof(key), nullptr)) {
    std::printf("[mirror_client] FAIL find \"%s\" - reboot after INF install?\n", kDriverString);
    rdm_dump_display_devices();
    return false;
  }
  if (device_name && device_name_len) {
    std::strncpy(device_name, name, device_name_len - 1);
    device_name[device_name_len - 1] = '\0';
  }

  // UltraVNC order: Attach.ToDesktop=1 in registry, then ChangeDisplaySettingsEx.
  // (Clearing to 0 then attaching — as we did briefly — fails on Win7.)
  write_attach_registry(key, 1);

  DEVMODEA dm{};
  dm.dmSize = sizeof(dm);
  dm.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;
  dm.dmBitsPerPel = 32;
  dm.dmPelsWidth = cx;
  dm.dmPelsHeight = cy;
  dm.dmPosition.x = 0;
  dm.dmPosition.y = 0;

  LONG code = ChangeDisplaySettingsExA(name, &dm, nullptr,
                                       CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
  if (code != DISP_CHANGE_SUCCESSFUL) {
    std::printf("[mirror_client] CDS attach failed code=%ld device=%s %ux%u\n", code, name, cx,
                cy);
    clear_attach_registry(key);
    return false;
  }
  code = ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, 0, nullptr);
  if (code != DISP_CHANGE_SUCCESSFUL && code != DISP_CHANGE_RESTART) {
    std::printf("[mirror_client] CDS apply failed code=%ld\n", code);
    clear_attach_registry(key);
    cds_detach_device(name);
    return false;
  }
  // Session stays attached; scrub registry to 0 so Device Manager PnP does not
  // persist/re-attach Road Desk Mirror (dual-mirror mouse freeze with VNC).
  clear_attach_registry(key);
  if (code == DISP_CHANGE_RESTART) {
    std::printf("[mirror_client] CDS asks for reboot - reboot then retry gm2\n");
  }
  return true;
}

bool rdm_detach_mirror(const char* device_name) {
  char name[128] = {};
  char key[512] = {};
  const bool found = find_mirror_device_ex(name, sizeof(name), key, sizeof(key), nullptr);
  clear_attach_registry(found ? key : nullptr);
  const char* cds_name = (device_name && device_name[0]) ? device_name : (found ? name : nullptr);
  if (!cds_name) {
    return true;
  }
  return cds_detach_device(cds_name);
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
