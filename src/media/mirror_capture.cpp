#include "mirror_capture.h"

#include "media_log.h"
#include "mirror_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

namespace {
// Sample-based mostly-black test (<= ~64k sampled pixels, cheap per frame).
bool frame_mostly_black(const std::vector<uint8_t>& bgra, int w, int h) {
  const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
  if (n == 0 || bgra.size() < n * 4u) {
    return false;
  }
  const size_t stride = (n > 65536u) ? (n / 65536u) : 1u;
  size_t nonblack = 0;
  size_t sampled = 0;
  for (size_t i = 0; i < n; i += stride) {
    const uint8_t* p = bgra.data() + i * 4u;
    if (p[0] > 16 || p[1] > 16 || p[2] > 16) {
      ++nonblack;
    }
    ++sampled;
  }
  return sampled > 0 && (nonblack * 200u) < sampled;
}

void dump_bgra_debug(const char* name, const std::vector<uint8_t>& bgra, int w, int h) {
  if (w <= 0 || h <= 0 || bgra.size() < static_cast<size_t>(w) * h * 4u) {
    return;
  }
  char dir[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, dir, MAX_PATH);
  char* slash = std::strrchr(dir, '\\');
  if (slash) {
    *slash = '\0';
  }
  std::snprintf(dir + std::strlen(dir), sizeof(dir) - std::strlen(dir), "\\debug");
  CreateDirectoryA(dir, nullptr);
  char path[MAX_PATH];
  std::snprintf(path, sizeof(path), "%s\\%s_%dx%d.bgra", dir, name, w, h);
  HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    DWORD wrote = 0;
    WriteFile(f, bgra.data(), static_cast<DWORD>(bgra.size()), &wrote, nullptr);
    CloseHandle(f);
  }
}
}  // namespace

namespace road_desk::replace {

SessionCapture::SessionCapture(CaptureMode mode) : mode_(mode) {
  dirty_buf_.resize(RDM_DIRTY_BUF_BYTES);
}

SessionCapture::~SessionCapture() {
  end();
}

bool SessionCapture::begin() {
  begun_ = true;
  have_frame_ = false;
  using_mirror_ = false;
  using_dxgi_ = false;

  if (mode_ == CaptureMode::Gdi) {
    road_desk::media::media_logf("media-capture", "capture begin: mode=Gdi");
    return true;
  }

  if (mode_ == CaptureMode::Dxgi) {
    if (dxgi_.begin()) {
      using_dxgi_ = true;
      road_desk::media::media_logf("media-capture", "capture begin: dxgi");
      return true;
    }
    // DXGI unavailable — keep session on GDI.
    road_desk::media::media_logf("media-capture", "capture begin: dxgi failed, gdi");
    return true;
  }

  // Auto / Mirror: XPDM Mirror (legacy). Auto on modern is resolved to Dxgi before begin.
  road_desk::media::media_logf("media-capture", "capture begin: trying mirror (mode=%d)",
                               static_cast<int>(mode_));
  if (begin_mirror()) {
    using_mirror_ = true;
    road_desk::media::media_logf("media-capture", "capture begin: mirror ok");
    return true;
  }
  using_mirror_ = false;
  road_desk::media::media_logf("media-capture", "capture begin: mirror failed, gdi fallback");
  return true;
}

void SessionCapture::end() {
  end_mirror();
  dxgi_.end();
  release_dib();
  using_mirror_ = false;
  using_dxgi_ = false;
  begun_ = false;
  have_frame_ = false;
}

bool SessionCapture::begin_mirror() {
  if (!rdm_attach_mirror(mirror_dev_, sizeof(mirror_dev_))) {
    road_desk::media::media_logf("media-capture", "mirror begin: attach failed err=%ld",
                                 rdm_last_attach_error());
    return false;
  }
  mirror_hdc_ = rdm_create_mirror_dc(mirror_dev_);
  if (!mirror_hdc_) {
    road_desk::media::media_logf("media-capture", "mirror begin: CreateDC(%s) failed err=%lu",
                                 mirror_dev_, static_cast<unsigned long>(GetLastError()));
    rdm_detach_mirror(mirror_dev_);
    mirror_dev_[0] = '\0';
    return false;
  }
  screen_dc_ = GetDC(nullptr);
  if (!screen_dc_) {
    road_desk::media::media_logf("media-capture", "mirror begin: GetDC failed err=%lu",
                                 static_cast<unsigned long>(GetLastError()));
    DeleteDC(static_cast<HDC>(mirror_hdc_));
    mirror_hdc_ = nullptr;
    rdm_detach_mirror(mirror_dev_);
    mirror_dev_[0] = '\0';
    return false;
  }
  return true;
}

void SessionCapture::end_mirror() {
  if (mirror_hdc_) {
    DeleteDC(static_cast<HDC>(mirror_hdc_));
    mirror_hdc_ = nullptr;
  }
  if (screen_dc_) {
    ReleaseDC(nullptr, static_cast<HDC>(screen_dc_));
    screen_dc_ = nullptr;
  }
  if (mirror_dev_[0]) {
    rdm_detach_mirror(mirror_dev_);
    mirror_dev_[0] = '\0';
  }
}

void SessionCapture::release_dib() {
  if (mem_dc_ && old_bmp_) {
    SelectObject(static_cast<HDC>(mem_dc_), static_cast<HGDIOBJ>(old_bmp_));
    old_bmp_ = nullptr;
  }
  if (dib_) {
    DeleteObject(static_cast<HBITMAP>(dib_));
    dib_ = nullptr;
  }
  dib_bits_ = nullptr;
  if (mem_dc_) {
    DeleteDC(static_cast<HDC>(mem_dc_));
    mem_dc_ = nullptr;
  }
  width_ = 0;
  height_ = 0;
}

bool SessionCapture::ensure_dib(int w, int h) {
  if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
    return false;
  }
  if (mem_dc_ && dib_ && width_ == w && height_ == h) {
    return true;
  }
  release_dib();
  HDC ref = screen_dc_ ? static_cast<HDC>(screen_dc_) : GetDC(nullptr);
  if (!ref) {
    return false;
  }
  mem_dc_ = CreateCompatibleDC(ref);
  if (!screen_dc_) {
    ReleaseDC(nullptr, ref);
  }
  if (!mem_dc_) {
    return false;
  }
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  dib_ = CreateDIBSection(static_cast<HDC>(mem_dc_), &bmi, DIB_RGB_COLORS, &dib_bits_, nullptr, 0);
  if (!dib_ || !dib_bits_) {
    release_dib();
    return false;
  }
  old_bmp_ = SelectObject(static_cast<HDC>(mem_dc_), static_cast<HBITMAP>(dib_));
  width_ = w;
  height_ = h;
  have_frame_ = false;
  return true;
}

bool SessionCapture::rect_in_bounds(int x, int y, int rw, int rh) const {
  if (rw <= 0 || rh <= 0 || x < 0 || y < 0) {
    return false;
  }
  if (x > width_ - rw || y > height_ - rh) {
    return false;
  }
  return true;
}

bool SessionCapture::blit_rect(int x, int y, int rw, int rh) {
  if (!mem_dc_ || !screen_dc_ || !rect_in_bounds(x, y, rw, rh)) {
    return false;
  }
  // Pixels from primary; dirty regions from Mirror ExtEscape.
  // No CAPTUREBLT: hardware cursor must not be baked into the frame — Viewer draws
  // a software cursor on Cursor channel (CAPTUREBLT causes visible cursor flicker).
  return BitBlt(static_cast<HDC>(mem_dc_), x, y, rw, rh, static_cast<HDC>(screen_dc_), x, y,
                SRCCOPY) != 0;
}

bool SessionCapture::capture(std::vector<uint8_t>* bgra, int* width, int* height,
                             std::vector<CaptureDirty>* dirties, std::vector<CaptureMove>* moves,
                             bool force_full_pixels) {
  if (!begun_ || !bgra || !width || !height || !dirties || !moves) {
    return false;
  }
  dirties->clear();
  moves->clear();
  if (using_mirror_) {
    return capture_mirror(bgra, width, height, dirties, force_full_pixels);
  }
  if (using_dxgi_) {
    if (dxgi_.capture(bgra, width, height, dirties, moves, force_full_pixels)) {
      if (!force_full_pixels) {
        if (frame_mostly_black(*bgra, *width, *height)) {
          ++dxgi_black_streak_;
        } else {
          dxgi_black_streak_ = 0;
        }
        // VMware SVGA DDA quirk: DuplicateOutput can silently deliver all-black
        // frames while the console shows a live desktop. After a sustained black
        // streak, probe GDI once — if GDI disagrees, drop DXGI for the session
        // and keep serving the real desktop via GDI.
        if (dxgi_black_streak_ >= 30 && (dxgi_black_streak_ % 30) == 0) {
          std::vector<uint8_t> probe;
          int pw = 0;
          int ph = 0;
          if (gdi_.capture(&probe, &pw, &ph) && !frame_mostly_black(probe, pw, ph)) {
            road_desk::media::media_logf("media-host",
                        "dxgi black streak=%u -> gdi fallback (DDA black-frame quirk)",
                        dxgi_black_streak_);
            dump_bgra_debug("gdi_probe", probe, pw, ph);
            dxgi_.end();
            using_dxgi_ = false;
            const bool gok = gdi_.capture(bgra, width, height);
            dump_bgra_debug("gdi_after_fallback", *bgra, *width, *height);
            return gok;
          }
        }
      }
      return true;
    }
    // Mode change / access lost and reinit failed — soft-fall to GDI for the session.
    dxgi_.end();
    using_dxgi_ = false;
  }
  return gdi_.capture(bgra, width, height);
}

bool SessionCapture::capture_gdi_region(std::vector<uint8_t>* bgra_full, int x, int y, int rw,
                                            int rh) {
  if (using_mirror_ || using_dxgi_) {
    return false;
  }
  return gdi_.capture_region(bgra_full, x, y, rw, rh);
}

bool SessionCapture::blit_rect_from_frame(int x, int y, int rw, int rh) {
  // Copy pixels from the driver framebuffer (frame_buf_) into the DIB at (x,y).
  // frame_buf_ layout: RdmFrameHeader followed by top-down rows of Pitch bytes.
  if (!dib_bits_ || frame_buf_.size() < sizeof(RdmFrameHeader)) {
    return false;
  }
  const auto* hdr = reinterpret_cast<const RdmFrameHeader*>(frame_buf_.data());
  const uint32_t fw = hdr->width;
  const uint32_t fh = hdr->height;
  const uint32_t fpitch = hdr->pitch;
  if (fw == 0 || fh == 0 || fpitch == 0) {
    return false;
  }
  if (x < 0 || y < 0 || rw <= 0 || rh <= 0 || x + rw > static_cast<int>(fw) ||
      y + rh > static_cast<int>(fh)) {
    return false;
  }
  const uint8_t* src = frame_buf_.data() + sizeof(RdmFrameHeader);
  // dest is top-down DIB, width_*4 per row (32bpp).
  uint8_t* dst = static_cast<uint8_t*>(dib_bits_);
  for (int row = 0; row < rh; ++row) {
    std::memcpy(dst + (static_cast<size_t>(y + row) * width_ + x) * 4u,
                src + (static_cast<size_t>(y + row) * fpitch + x) * 4u,
                static_cast<size_t>(rw) * 4u);
  }
  return true;
}

bool SessionCapture::capture_mirror(std::vector<uint8_t>* bgra, int* width, int* height,
                                    std::vector<CaptureDirty>* dirties, bool force_full_pixels) {
  const int sw = GetSystemMetrics(SM_CXSCREEN);
  const int sh = GetSystemMetrics(SM_CYSCREEN);
  if (!ensure_dib(sw, sh)) {
    return false;
  }

  RdmInfo info{};
  if (rdm_escape_get_info(static_cast<HDC>(mirror_hdc_), &info) && info.width > 0 &&
      info.height > 0) {
    if (static_cast<int>(info.width) != sw || static_cast<int>(info.height) != sh) {
      have_frame_ = false;
    }
  }

  // Read the full framebuffer from the mirror driver. Works even when GetDC
  // cannot BitBlt the screen (lock/login desktop). Fall back to screen DC blit.
  const uint32_t frame_cap =
      static_cast<uint32_t>(sizeof(RdmFrameHeader) + sw * sh * 4u + 64u);
  frame_buf_.resize(frame_cap);
  const uint32_t got = rdm_escape_get_frame(static_cast<HDC>(mirror_hdc_), frame_buf_.data(),
                                            frame_cap);
  const bool have_frame_buf = got >= sizeof(RdmFrameHeader);

  const uint32_t n = rdm_escape_get_dirty(static_cast<HDC>(mirror_hdc_), dirty_buf_.data(),
                                          static_cast<uint32_t>(dirty_buf_.size()));
  bool need_full = !have_frame_ || force_full_pixels;
  if (n >= sizeof(RdmDirtyHeader)) {
    const auto* hdr = reinterpret_cast<const RdmDirtyHeader*>(dirty_buf_.data());
    const auto* rects = reinterpret_cast<const RdmRect*>(hdr + 1);
    for (uint32_t i = 0; i < hdr->count; ++i) {
      const RdmRect& r = rects[i];
      if (r.w <= 0 || r.h <= 0) {
        continue;
      }
      if (r.x == 0 && r.y == 0 && r.w == width_ && r.h == height_) {
        need_full = true;
        break;
      }
      if (rect_in_bounds(r.x, r.y, r.w, r.h)) {
        if (!force_full_pixels) {
          dirties->push_back(CaptureDirty{r.x, r.y, r.w, r.h});
        }
      } else {
        need_full = true;
        break;
      }
    }
  } else if (!have_frame_) {
    need_full = true;
  }

  auto blit = [&](int x, int y, int rw, int rh) -> bool {
    if (have_frame_buf) {
      return blit_rect_from_frame(x, y, rw, rh);
    }
    return blit_rect(x, y, rw, rh);
  };

  if (force_full_pixels) {
    dirties->clear();
    if (!blit(0, 0, width_, height_)) {
      return false;
    }
  } else if (need_full) {
    dirties->clear();
    dirties->push_back(CaptureDirty{0, 0, width_, height_});
    if (!blit(0, 0, width_, height_)) {
      return false;
    }
  } else if (dirties->empty()) {
    *width = width_;
    *height = height_;
    const size_t bytes = static_cast<size_t>(width_) * height_ * 4u;
    bgra->resize(bytes);
    if (dib_bits_ && have_frame_) {
      std::memcpy(bgra->data(), dib_bits_, bytes);
    }
    return true;
  } else {
    for (const CaptureDirty& r : *dirties) {
      if (!blit(r.x, r.y, r.w, r.h)) {
        return false;
      }
    }
  }

  const size_t bytes = static_cast<size_t>(width_) * height_ * 4u;
  bgra->resize(bytes);
  std::memcpy(bgra->data(), dib_bits_, bytes);
  *width = width_;
  *height = height_;
  have_frame_ = true;
  return true;
}

}  // namespace road_desk::replace
