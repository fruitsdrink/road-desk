// Probe Road Desk Mirror control device (GM1).
// No driver => non-zero exit with clear message (expected before install).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>

#include <cstdio>
#include <cstring>

#include "mirror_abi.h"

namespace {

void print_last_error(const char* what) {
  const DWORD e = GetLastError();
  char* msg = nullptr;
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, e, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                 reinterpret_cast<LPSTR>(&msg), 0, nullptr);
  std::printf("[mirror_probe] %s failed gle=%lu %s\n", what,
              static_cast<unsigned long>(e), msg ? msg : "");
  if (msg) {
    LocalFree(msg);
  }
}

}  // namespace

int main() {
  std::printf("[mirror_probe] abi=%u device=%s\n", ROAD_DESK_MIRROR_ABI_VERSION,
              ROAD_DESK_MIRROR_DEVICE_NAME_USER);

  HANDLE h = CreateFileA(ROAD_DESK_MIRROR_DEVICE_NAME_USER, GENERIC_READ | GENERIC_WRITE,
                         0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    print_last_error("CreateFile");
    std::printf("[mirror_probe] FAIL not_loaded (install rdmmirror + test-sign; see docs/)\n");
    return 2;
  }

  RdmStatus st{};
  DWORD got = 0;
  if (!DeviceIoControl(h, IOCTL_RDM_GET_STATUS, nullptr, 0, &st, sizeof(st), &got, nullptr)) {
    print_last_error("IOCTL_RDM_GET_STATUS");
    CloseHandle(h);
    return 3;
  }
  if (got < sizeof(st)) {
    std::printf("[mirror_probe] FAIL status_short got=%lu\n", static_cast<unsigned long>(got));
    CloseHandle(h);
    return 4;
  }

  std::printf("[mirror_probe] status abi=%u flags=0x%x build=%u\n", st.abi_version, st.flags,
              st.driver_build);

  RdmInfo info{};
  got = 0;
  if (!DeviceIoControl(h, IOCTL_RDM_GET_INFO, nullptr, 0, &info, sizeof(info), &got, nullptr)) {
    print_last_error("IOCTL_RDM_GET_INFO");
    CloseHandle(h);
    return 5;
  }

  std::printf("[mirror_probe] info %ux%u pitch=%u format=%u\n", info.width, info.height,
              info.pitch, info.format);

  if ((st.flags & kRdmStatusLoaded) == 0) {
    std::printf("[mirror_probe] FAIL missing LOADED flag\n");
    CloseHandle(h);
    return 6;
  }

  std::printf("[mirror_probe] GM1_LOADED ok (capture_ready=%d xpdm=%d)\n",
              (st.flags & kRdmStatusCaptureReady) ? 1 : 0,
              (st.flags & kRdmStatusXpdmAttached) ? 1 : 0);
  CloseHandle(h);
  return 0;
}
