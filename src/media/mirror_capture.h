#pragma once

#include "capture.h"
#include "capture_resolve.h"
#include "dxgi_capture.h"

#include <cstdint>
#include <vector>

namespace road_desk::replace {

// Session capture: Mirror / DXGI dirties + pixels, or GDI full frame.
class SessionCapture {
 public:
  explicit SessionCapture(CaptureMode mode);
  ~SessionCapture();

  SessionCapture(const SessionCapture&) = delete;
  SessionCapture& operator=(const SessionCapture&) = delete;

  // Call once after auth. Always keeps session alive (falls back to GDI).
  bool begin();
  void end();

  bool using_mirror() const { return using_mirror_; }
  bool using_dxgi() const { return using_dxgi_; }
  // Native dirty rects (Mirror or DXGI) — mux skips CPU block-diff when set.
  bool provides_dirties() const { return using_mirror_ || using_dxgi_; }
  const char* backend_name() const {
    if (using_mirror_) {
      return "mirror";
    }
    if (using_dxgi_) {
      return "dxgi";
    }
    return "gdi";
  }
  const char* mirror_device() const { return mirror_dev_; }

  // Fills BGRA top-down frame.
  // Mirror/DXGI: fills dirties (empty => idle). DXGI also fills moves (CopyRect).
  // GDI: dirties/moves cleared; caller CPU-diffs.
  // force_full_pixels: refresh entire desktop; dirties/moves cleared so caller CPU-diffs.
  bool capture(std::vector<uint8_t>* bgra_top_down, int* width, int* height,
               std::vector<CaptureDirty>* dirties, std::vector<CaptureMove>* moves,
               bool force_full_pixels = false);

  // GDI-only: BitBlt a sub-rect into an existing full-frame buffer. Regions
  // outside stay stale; the caller diffs only the rect. Mirror/DXGI return
  // false (the loop should keep using full capture for those backends).
  bool capture_gdi_region(std::vector<uint8_t>* bgra_full, int x, int y, int rw, int rh);

 private:
  bool begin_mirror();
  void end_mirror();
  bool capture_mirror(std::vector<uint8_t>* bgra, int* width, int* height,
                      std::vector<CaptureDirty>* dirties, bool force_full_pixels);
  // Copy rect pixels from the driver framebuffer (frame_buf_) into dib_bits_.
  bool blit_rect_from_frame(int x, int y, int rw, int rh);
  bool ensure_dib(int w, int h);
  void release_dib();
  bool blit_rect(int x, int y, int rw, int rh);
  bool rect_in_bounds(int x, int y, int rw, int rh) const;

  CaptureMode mode_;
  bool using_mirror_ = false;
  bool using_dxgi_ = false;
  bool begun_ = false;
  bool have_frame_ = false;
  // Consecutive all-black DXGI frames; detects VMware SVGA DDA black-frame quirk
  // and drops to GDI (which shows the real console desktop).
  unsigned dxgi_black_streak_ = 0;
  char mirror_dev_[128] = {};
  void* mirror_hdc_ = nullptr;  // HDC
  void* screen_dc_ = nullptr;
  void* mem_dc_ = nullptr;
  void* dib_ = nullptr;
  void* old_bmp_ = nullptr;
  void* dib_bits_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  DesktopCapture gdi_;
  DxgiCapture dxgi_;
  std::vector<uint8_t> dirty_buf_;
  std::vector<uint8_t> frame_buf_;  // scratch for rdm_escape_get_frame
};

}  // namespace road_desk::replace
