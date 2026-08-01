#include "mirror_client.h"

#include <cstring>
#include <cstdio>

namespace {

const char* kDriverString = ROAD_DESK_MIRROR_DISPLAY_NAME;

bool device_string_is_ours(const char* device_string) {
  if (!device_string || !device_string[0]) {
    return false;
  }
  if (std::strcmp(device_string, kDriverString) == 0) {
    return true;
  }
  // Device Manager friendly name vs EnumDisplayDevices DeviceString can differ
  // slightly; accept substring match on stable brand token.
  return std::strstr(device_string, "Road Desk Mirror") != nullptr;
}

bool get_primary_size(DWORD* cx, DWORD* cy) {
  DEVMODEA dm{};
  dm.dmSize = sizeof(dm);
  DISPLAY_DEVICEA dd{};
  dd.cb = sizeof(dd);
  for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i) {
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
  if (!out_name || out_len < 16) {
    return false;
  }
  DISPLAY_DEVICEA dd{};
  dd.cb = sizeof(dd);
  for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i) {
    if (device_string_is_ours(dd.DeviceString)) {
      std::strncpy(out_name, dd.DeviceName, out_len - 1);
      out_name[out_len - 1] = '\0';
      return true;
    }
  }
  return false;
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
  if (!rdm_find_mirror_device(name, sizeof(name))) {
    std::printf("[mirror_client] FAIL find \"%s\" — Device Manager may show it "
                "but GDI has not loaded the video stack yet. Reboot, then retry.\n",
                kDriverString);
    rdm_dump_display_devices();
    return false;
  }
  if (device_name && device_name_len) {
    std::strncpy(device_name, name, device_name_len - 1);
    device_name[device_name_len - 1] = '\0';
  }

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
    std::printf("[mirror_client] CDS update registry failed code=%ld device=%s %ux%u\n",
                code, name, cx, cy);
    return false;
  }
  code = ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, 0, nullptr);
  if (code != DISP_CHANGE_SUCCESSFUL && code != DISP_CHANGE_RESTART) {
    std::printf("[mirror_client] CDS apply failed code=%ld\n", code);
    return false;
  }
  if (code == DISP_CHANGE_RESTART) {
    std::printf("[mirror_client] CDS asks for reboot — reboot then retry gm2\n");
  }
  return true;
}

bool rdm_detach_mirror(const char* device_name) {
  if (!device_name || !device_name[0]) {
    return false;
  }
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
