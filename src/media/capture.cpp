#include "capture.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

namespace road_desk::replace {

DesktopCapture::DesktopCapture() = default;

DesktopCapture::~DesktopCapture() {
  release();
}

void DesktopCapture::release() {
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
  if (screen_dc_) {
    ReleaseDC(nullptr, static_cast<HDC>(screen_dc_));
    screen_dc_ = nullptr;
  }
  width_ = 0;
  height_ = 0;
}

bool DesktopCapture::ensure(int w, int h) {
  if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
    return false;
  }
  if (screen_dc_ && mem_dc_ && dib_ && width_ == w && height_ == h) {
    return true;
  }
  release();

  screen_dc_ = GetDC(nullptr);
  if (!screen_dc_) {
    return false;
  }
  mem_dc_ = CreateCompatibleDC(static_cast<HDC>(screen_dc_));
  if (!mem_dc_) {
    release();
    return false;
  }

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  dib_ = CreateDIBSection(static_cast<HDC>(mem_dc_), &bmi, DIB_RGB_COLORS, &dib_bits_, nullptr, 0);
  if (!dib_ || !dib_bits_) {
    release();
    return false;
  }
  old_bmp_ = SelectObject(static_cast<HDC>(mem_dc_), static_cast<HBITMAP>(dib_));
  width_ = w;
  height_ = h;
  return true;
}

bool DesktopCapture::capture_region(std::vector<uint8_t>* bgra_top_down, int x, int y, int rw,
                                         int rh) {
  if (!bgra_top_down) {
    return false;
  }
  const int w = GetSystemMetrics(SM_CXSCREEN);
  const int h = GetSystemMetrics(SM_CYSCREEN);
  if (!ensure(w, h)) {
    return false;
  }
  if (x < 0) {
    x = 0;
  }
  if (y < 0) {
    y = 0;
  }
  if (x + rw > w) {
    rw = w - x;
  }
  if (y + rh > h) {
    rh = h - y;
  }
  if (rw <= 0 || rh <= 0) {
    return false;
  }
  // SRCCOPY only — keep OS cursor out of framebuffer (Viewer uses soft cursor).
  if (!BitBlt(static_cast<HDC>(mem_dc_), x, y, rw, rh, static_cast<HDC>(screen_dc_), x, y,
              SRCCOPY)) {
    return false;
  }
  const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
  if (bgra_top_down->size() < bytes) {
    bgra_top_down->resize(bytes);
  }
  const uint8_t* src = static_cast<const uint8_t*>(dib_bits_);
  uint8_t* dst = bgra_top_down->data();
  for (int row = 0; row < rh; ++row) {
    std::memcpy(dst + (static_cast<size_t>(y + row) * w + x) * 4u,
                src + (static_cast<size_t>(y + row) * w + x) * 4u,
                static_cast<size_t>(rw) * 4u);
  }
  return true;
}

bool DesktopCapture::capture(std::vector<uint8_t>* bgra_top_down, int* width, int* height) {
  if (!bgra_top_down || !width || !height) {
    return false;
  }
  const int w = GetSystemMetrics(SM_CXSCREEN);
  const int h = GetSystemMetrics(SM_CYSCREEN);
  if (!ensure(w, h)) {
    return false;
  }
  // SRCCOPY only — keep OS cursor out of framebuffer (Viewer uses soft cursor).
  if (!BitBlt(static_cast<HDC>(mem_dc_), 0, 0, w, h, static_cast<HDC>(screen_dc_), 0, 0,
              SRCCOPY)) {
    return false;
  }
  const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
  bgra_top_down->resize(bytes);
  std::memcpy(bgra_top_down->data(), dib_bits_, bytes);
  *width = w;
  *height = h;
  return true;
}

}  // namespace road_desk::replace
