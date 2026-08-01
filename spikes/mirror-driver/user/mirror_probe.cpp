// GM1: open \\.\RoadDeskMirror control device.
// GM2: attach XPDM mirror, poll ExtEscape dirty rects.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "mirror_abi.h"
#include "mirror_client.h"

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

int probe_control() {
  std::printf("[mirror_probe] abi=%u device=%s\n", ROAD_DESK_MIRROR_ABI_VERSION,
              ROAD_DESK_MIRROR_DEVICE_NAME_USER);

  HANDLE h = CreateFileA(ROAD_DESK_MIRROR_DEVICE_NAME_USER, GENERIC_READ | GENERIC_WRITE,
                         0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    print_last_error("CreateFile");
    std::printf("[mirror_probe] FAIL not_loaded (GM1 control; see docs/sc-workflow.md)\n");
    return 2;
  }

  RdmStatus st{};
  DWORD got = 0;
  if (!DeviceIoControl(h, IOCTL_RDM_GET_STATUS, nullptr, 0, &st, sizeof(st), &got, nullptr) ||
      got < sizeof(st)) {
    print_last_error("IOCTL_RDM_GET_STATUS");
    CloseHandle(h);
    return 3;
  }
  std::printf("[mirror_probe] status abi=%u flags=0x%x build=%u\n", st.abi_version, st.flags,
              st.driver_build);
  CloseHandle(h);
  if ((st.flags & kRdmStatusLoaded) == 0) {
    return 6;
  }
  std::printf("[mirror_probe] GM1_LOADED ok\n");
  return 0;
}

bool save_rect_bmp(const char* path, int x, int y, int w, int h) {
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
    return false;
  }
  HDC screen = GetDC(nullptr);
  if (!screen) {
    return false;
  }
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) {
      DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return false;
  }
  HGDIOBJ old = SelectObject(mem, dib);
  BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY);

#pragma pack(push, 1)
  struct BmpFileHdr {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
  };
#pragma pack(pop)
  const uint32_t pix = static_cast<uint32_t>(w) * h * 4u;
  BmpFileHdr fh{};
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof(BmpFileHdr) + sizeof(BITMAPINFOHEADER);
  fh.bfSize = fh.bfOffBits + pix;
  FILE* fp = nullptr;
  if (fopen_s(&fp, path, "wb") != 0 || !fp) {
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return false;
  }
  fwrite(&fh, 1, sizeof(fh), fp);
  fwrite(&bmi.bmiHeader, 1, sizeof(BITMAPINFOHEADER), fp);
  fwrite(bits, 1, pix, fp);
  fclose(fp);
  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return true;
}

int probe_gm2(int seconds) {
  char dev[128] = {};
  std::printf("[mirror_probe] GM2 attach (%s)\n", ROAD_DESK_MIRROR_DISPLAY_NAME);
  if (!rdm_attach_mirror(dev, sizeof(dev))) {
    std::printf("[mirror_probe] FAIL attach (install rdm_xpdm.inf + reboot?; see docs/gm2-checklist.md)\n");
    return 10;
  }
  std::printf("[mirror_probe] attached device=%s\n", dev);

  HDC hdc = rdm_create_mirror_dc(dev);
  if (!hdc) {
    print_last_error("CreateDC mirror");
    rdm_detach_mirror(dev);
    return 11;
  }

  RdmInfo info{};
  if (!rdm_escape_get_info(hdc, &info)) {
    std::printf("[mirror_probe] WARN GET_INFO escape failed (may still get dirty)\n");
  } else {
    std::printf("[mirror_probe] info %ux%u pitch=%u\n", info.width, info.height, info.pitch);
  }

  std::vector<uint8_t> buf(RDM_DIRTY_BUF_BYTES);
  uint32_t total_rects = 0;
  uint32_t polls_with_dirty = 0;
  const DWORD t0 = GetTickCount();
  const DWORD limit_ms = static_cast<DWORD>(seconds > 0 ? seconds : 8) * 1000u;
  std::printf("[mirror_probe] polling dirty for %u s - drag a window / refresh UI\n",
              limit_ms / 1000u);

  while (GetTickCount() - t0 < limit_ms) {
    const uint32_t n = rdm_escape_get_dirty(hdc, buf.data(), static_cast<uint32_t>(buf.size()));
    if (n >= sizeof(RdmDirtyHeader)) {
      const auto* hdr = reinterpret_cast<const RdmDirtyHeader*>(buf.data());
      if (hdr->count > 0) {
        ++polls_with_dirty;
        total_rects += hdr->count;
        const auto* r = reinterpret_cast<const RdmRect*>(hdr + 1);
        std::printf("[mirror_probe] dirty seq=%u count=%u e.g. %d,%d %dx%d\n", hdr->seq,
                    hdr->count, r[0].x, r[0].y, r[0].w, r[0].h);
        if (polls_with_dirty == 1) {
          char bmp[MAX_PATH];
          GetModuleFileNameA(nullptr, bmp, MAX_PATH);
          char* slash = std::strrchr(bmp, '\\');
          if (slash) {
            std::strcpy(slash + 1, "dirty_sample.bmp");
          } else {
            std::strcpy(bmp, "dirty_sample.bmp");
          }
          if (save_rect_bmp(bmp, r[0].x, r[0].y, r[0].w, r[0].h)) {
            std::printf("[mirror_probe] wrote %s\n", bmp);
          }
        }
      }
    }
    Sleep(100);
  }

  DeleteDC(hdc);
  rdm_detach_mirror(dev);

  if (total_rects == 0) {
    std::printf("[mirror_probe] FAIL GM2 no dirty rects seen (is mirror hooked? move windows)\n");
    return 12;
  }
  std::printf("[mirror_probe] GM2_DIRTY ok rects=%u polls=%u\n", total_rects, polls_with_dirty);
  return 0;
}

void usage() {
  std::printf("Usage:\n");
  std::printf("  mirror_probe.exe              GM1 control device check\n");
  std::printf("  mirror_probe.exe gm2 [sec]    attach XPDM, poll dirty (default 8s)\n");
  std::printf("  mirror_probe.exe attach       attach only\n");
  std::printf("  mirror_probe.exe detach       detach only\n");
  std::printf("  mirror_probe.exe list         EnumDisplayDevices dump\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "/?") == 0)) {
    usage();
    return 0;
  }
  if (argc >= 2 && std::strcmp(argv[1], "gm2") == 0) {
    int sec = 8;
    if (argc >= 3) {
      sec = std::atoi(argv[2]);
    }
    return probe_gm2(sec);
  }
  if (argc >= 2 && std::strcmp(argv[1], "list") == 0) {
    rdm_dump_display_devices();
    char dev[128] = {};
    if (rdm_find_mirror_device(dev, sizeof(dev))) {
      std::printf("[mirror_probe] found mirror device=%s\n", dev);
      return 0;
    }
    std::printf("[mirror_probe] mirror not in EnumDisplayDevices - reboot after INF install\n");
    return 1;
  }
  if (argc >= 2 && std::strcmp(argv[1], "attach") == 0) {
    char dev[128] = {};
    if (!rdm_attach_mirror(dev, sizeof(dev))) {
      std::printf("[mirror_probe] FAIL attach\n");
      return 10;
    }
    std::printf("[mirror_probe] attached %s\n", dev);
    return 0;
  }
  if (argc >= 2 && std::strcmp(argv[1], "detach") == 0) {
    char dev[128] = {};
    if (!rdm_find_mirror_device(dev, sizeof(dev)) || !rdm_detach_mirror(dev)) {
      std::printf("[mirror_probe] FAIL detach\n");
      return 11;
    }
    std::printf("[mirror_probe] detached %s\n", dev);
    return 0;
  }
  return probe_control();
}
