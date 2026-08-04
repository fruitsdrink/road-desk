#pragma once

#include <cstdint>
#include <vector>

namespace road_desk::replace {

struct CaptureDirty {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

// Desktop move (DXGI MoveRect / protocol CopyRect). Src → dst, same size.
struct CaptureMove {
  int sx = 0;
  int sy = 0;
  int dx = 0;
  int dy = 0;
  int w = 0;
  int h = 0;
};

// GDI primary-desktop capture (SRCCOPY, no CAPTUREBLT). Output top-down BGRA.
class DesktopCapture {
 public:
  DesktopCapture();
  ~DesktopCapture();

  DesktopCapture(const DesktopCapture&) = delete;
  DesktopCapture& operator=(const DesktopCapture&) = delete;

  bool capture(std::vector<uint8_t>* bgra_top_down, int* width, int* height);

 private:
  void release();
  bool ensure(int w, int h);

  void* screen_dc_ = nullptr;  // HDC
  void* mem_dc_ = nullptr;
  void* dib_ = nullptr;        // HBITMAP
  void* old_bmp_ = nullptr;
  void* dib_bits_ = nullptr;
  int width_ = 0;
  int height_ = 0;
};

}  // namespace road_desk::replace
