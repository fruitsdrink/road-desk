#pragma once

#include "capture.h"

#include <cstdint>
#include <vector>

namespace road_desk::replace {

struct CaptureDirty {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

enum class CaptureMode {
  Auto,    // try Mirror, else GDI
  Mirror,  // require Mirror attach
  Gdi      // force GDI + CPU dirty
};

// Session capture: Mirror ExtEscape dirties + blit pixels, or GDI full frame.
class SessionCapture {
 public:
  explicit SessionCapture(CaptureMode mode);
  ~SessionCapture();

  SessionCapture(const SessionCapture&) = delete;
  SessionCapture& operator=(const SessionCapture&) = delete;

  // Call once after auth. Returns false only if Mirror mode required and attach fails.
  bool begin();
  void end();

  bool using_mirror() const { return using_mirror_; }
  const char* backend_name() const { return using_mirror_ ? "mirror" : "gdi"; }
  const char* mirror_device() const { return mirror_dev_; }

  // Fills BGRA top-down frame.
  // Mirror: also fills dirties (empty => idle). GDI: dirties cleared; caller CPU-diffs.
  // force_full_pixels: blit entire desktop (still drains ExtEscape). Caller should CPU-diff;
  // dirties will be cleared so empty means "use pixel compare", not idle.
  bool capture(std::vector<uint8_t>* bgra_top_down, int* width, int* height,
               std::vector<CaptureDirty>* dirties, bool force_full_pixels = false);

 private:
  bool begin_mirror();
  void end_mirror();
  bool capture_mirror(std::vector<uint8_t>* bgra, int* width, int* height,
                      std::vector<CaptureDirty>* dirties, bool force_full_pixels);
  bool ensure_dib(int w, int h);
  void release_dib();
  bool blit_rect(int x, int y, int rw, int rh);
  bool rect_in_bounds(int x, int y, int rw, int rh) const;

  CaptureMode mode_;
  bool using_mirror_ = false;
  bool begun_ = false;
  bool have_frame_ = false;
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
  std::vector<uint8_t> dirty_buf_;
};

CaptureMode parse_capture_mode(const char* s);

}  // namespace road_desk::replace
