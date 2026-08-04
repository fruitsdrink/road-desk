#pragma once

#include "capture.h"

#include <cstdint>
#include <vector>

namespace road_desk::replace {

// DXGI Desktop Duplication (Win8+). Dynamically loads dxgi/d3d11 — safe to link into Win7 builds.
class DxgiCapture {
 public:
  DxgiCapture();
  ~DxgiCapture();

  DxgiCapture(const DxgiCapture&) = delete;
  DxgiCapture& operator=(const DxgiCapture&) = delete;

  // false = unavailable (caller falls back to GDI).
  bool begin();
  void end();

  bool active() const { return dup_ != nullptr; }

  // Same contract as SessionCapture Mirror path: full BGRA + dirties (empty = idle).
  // moves: DXGI MoveRects mapped to CopyRect (process before dirties on Viewer).
  // force_full_pixels: refresh full buffer and clear dirties/moves (caller CPU-diffs).
  bool capture(std::vector<uint8_t>* bgra_top_down, int* width, int* height,
               std::vector<CaptureDirty>* dirties, std::vector<CaptureMove>* moves,
               bool force_full_pixels);

 private:
  bool load_apis();
  void unload_apis();
  bool create_duplication();
  void release_duplication();
  bool reinit();

  void* factory_ = nullptr;   // IDXGIFactory1*
  void* device_ = nullptr;    // ID3D11Device*
  void* context_ = nullptr;   // ID3D11DeviceContext*
  void* output1_ = nullptr;   // IDXGIOutput1*
  void* dup_ = nullptr;       // IDXGIOutputDuplication*
  void* staging_ = nullptr;   // ID3D11Texture2D*
  void* dxgi_mod_ = nullptr;  // HMODULE
  void* d3d_mod_ = nullptr;   // HMODULE

  int width_ = 0;
  int height_ = 0;
  bool have_frame_ = false;
  std::vector<uint8_t> frame_;
  std::vector<uint8_t> scratch_;
  std::vector<uint8_t> move_meta_;
  std::vector<uint8_t> dirty_meta_;
};

}  // namespace road_desk::replace
