#include "mirror_capture.h"

#include "mirror_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

namespace road_desk::replace {

CaptureMode parse_capture_mode(const char* s) {
  if (!s || !s[0]) {
    return CaptureMode::Auto;
  }
  if (_stricmp(s, "gdi") == 0) {
    return CaptureMode::Gdi;
  }
  if (_stricmp(s, "mirror") == 0) {
    return CaptureMode::Mirror;
  }
  return CaptureMode::Auto;
}

SessionCapture::SessionCapture(CaptureMode mode) : mode_(mode) {
  dirty_buf_.resize(RDM_DIRTY_BUF_BYTES);
}

SessionCapture::~SessionCapture() {
  end();
}

bool SessionCapture::begin() {
  begun_ = true;
  have_frame_ = false;
  if (mode_ == CaptureMode::Gdi) {
    using_mirror_ = false;
    return true;
  }
  if (begin_mirror()) {
    using_mirror_ = true;
    return true;
  }
  // Mirror attach failed — keep session alive on GDI (Viewer must not flash-exit).
  using_mirror_ = false;
  return true;
}

void SessionCapture::end() {
  end_mirror();
  release_dib();
  using_mirror_ = false;
  begun_ = false;
  have_frame_ = false;
}

bool SessionCapture::begin_mirror() {
  if (!rdm_attach_mirror(mirror_dev_, sizeof(mirror_dev_))) {
    return false;
  }
  mirror_hdc_ = rdm_create_mirror_dc(mirror_dev_);
  if (!mirror_hdc_) {
    rdm_detach_mirror(mirror_dev_);
    mirror_dev_[0] = '\0';
    return false;
  }
  screen_dc_ = GetDC(nullptr);
  if (!screen_dc_) {
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
                             std::vector<CaptureDirty>* dirties, bool force_full_pixels) {
  if (!begun_ || !bgra || !width || !height || !dirties) {
    return false;
  }
  dirties->clear();
  if (using_mirror_) {
    return capture_mirror(bgra, width, height, dirties, force_full_pixels);
  }
  return gdi_.capture(bgra, width, height);
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

  if (force_full_pixels) {
    // Z-order / activate may change primary pixels without Mirror dirty hooks.
    // Full blit + caller CPU-diff closes those holes (e.g. click title bar to raise).
    dirties->clear();
    if (!blit_rect(0, 0, width_, height_)) {
      return false;
    }
  } else if (need_full) {
    dirties->clear();
    dirties->push_back(CaptureDirty{0, 0, width_, height_});
    if (!blit_rect(0, 0, width_, height_)) {
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
      if (!blit_rect(r.x, r.y, r.w, r.h)) {
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
