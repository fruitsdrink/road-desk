#include "dxgi_capture.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstring>

namespace road_desk::replace {
namespace {

using CreateDXGIFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
using D3D11CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                             const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                             D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

template <typename T>
void safe_release(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

bool rect_to_dirty(const RECT& r, int desk_w, int desk_h, CaptureDirty* out) {
  if (!out) {
    return false;
  }
  const int x = r.left;
  const int y = r.top;
  const int w = r.right - r.left;
  const int h = r.bottom - r.top;
  if (w <= 0 || h <= 0 || x < 0 || y < 0) {
    return false;
  }
  if (x > desk_w - w || y > desk_h - h) {
    return false;
  }
  *out = CaptureDirty{x, y, w, h};
  return true;
}

bool move_to_capture(const DXGI_OUTDUPL_MOVE_RECT& m, int desk_w, int desk_h, CaptureMove* out) {
  if (!out) {
    return false;
  }
  const int dx = m.DestinationRect.left;
  const int dy = m.DestinationRect.top;
  const int w = m.DestinationRect.right - m.DestinationRect.left;
  const int h = m.DestinationRect.bottom - m.DestinationRect.top;
  const int sx = m.SourcePoint.x;
  const int sy = m.SourcePoint.y;
  if (w <= 0 || h <= 0 || dx < 0 || dy < 0 || sx < 0 || sy < 0) {
    return false;
  }
  if (dx > desk_w - w || dy > desk_h - h || sx > desk_w - w || sy > desk_h - h) {
    return false;
  }
  *out = CaptureMove{sx, sy, dx, dy, w, h};
  return true;
}

}  // namespace

DxgiCapture::DxgiCapture() = default;

DxgiCapture::~DxgiCapture() {
  end();
}

bool DxgiCapture::load_apis() {
  if (dxgi_mod_ && d3d_mod_) {
    return true;
  }
  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  HMODULE d3d = LoadLibraryW(L"d3d11.dll");
  if (!dxgi || !d3d) {
    if (dxgi) {
      FreeLibrary(dxgi);
    }
    if (d3d) {
      FreeLibrary(d3d);
    }
    return false;
  }
  if (!GetProcAddress(dxgi, "CreateDXGIFactory1") || !GetProcAddress(d3d, "D3D11CreateDevice")) {
    FreeLibrary(dxgi);
    FreeLibrary(d3d);
    return false;
  }
  dxgi_mod_ = dxgi;
  d3d_mod_ = d3d;
  return true;
}

void DxgiCapture::unload_apis() {
  if (dxgi_mod_) {
    FreeLibrary(static_cast<HMODULE>(dxgi_mod_));
    dxgi_mod_ = nullptr;
  }
  if (d3d_mod_) {
    FreeLibrary(static_cast<HMODULE>(d3d_mod_));
    d3d_mod_ = nullptr;
  }
}

bool DxgiCapture::create_duplication() {
  release_duplication();
  if (!load_apis()) {
    return false;
  }

  const auto create_factory = reinterpret_cast<CreateDXGIFactory1Fn>(
      GetProcAddress(static_cast<HMODULE>(dxgi_mod_), "CreateDXGIFactory1"));
  const auto create_device = reinterpret_cast<D3D11CreateDeviceFn>(
      GetProcAddress(static_cast<HMODULE>(d3d_mod_), "D3D11CreateDevice"));
  if (!create_factory || !create_device) {
    return false;
  }

  IDXGIFactory1* factory = nullptr;
  if (FAILED(create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) ||
      !factory) {
    return false;
  }

  // Prefer the output whose desktop origin is (0,0) — primary for SM_CX/YSCREEN inject.
  IDXGIAdapter1* adapter = nullptr;
  IDXGIOutput* output = nullptr;
  IDXGIOutput1* output1 = nullptr;
  IDXGIAdapter1* best_adapter = nullptr;
  IDXGIOutput* best_output = nullptr;
  for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
    for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi) {
      DXGI_OUTPUT_DESC desc{};
      if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop &&
          desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0) {
        best_adapter = adapter;
        best_output = output;
        adapter = nullptr;
        output = nullptr;
        break;
      }
      safe_release(output);
    }
    safe_release(adapter);
    if (best_output) {
      break;
    }
  }
  if (!best_output) {
    // Fallback: adapter 0 / output 0.
    if (FAILED(factory->EnumAdapters1(0, &best_adapter)) || !best_adapter ||
        FAILED(best_adapter->EnumOutputs(0, &best_output)) || !best_output) {
      safe_release(best_adapter);
      safe_release(factory);
      return false;
    }
  }

  DXGI_OUTPUT_DESC odesc{};
  if (FAILED(best_output->GetDesc(&odesc))) {
    safe_release(best_output);
    safe_release(best_adapter);
    safe_release(factory);
    return false;
  }
  const int w = odesc.DesktopCoordinates.right - odesc.DesktopCoordinates.left;
  const int h = odesc.DesktopCoordinates.bottom - odesc.DesktopCoordinates.top;
  if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
    safe_release(best_output);
    safe_release(best_adapter);
    safe_release(factory);
    return false;
  }

  if (FAILED(best_output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1))) ||
      !output1) {
    safe_release(best_output);
    safe_release(best_adapter);
    safe_release(factory);
    return false;
  }
  safe_release(best_output);

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0};
  HRESULT hr =
      create_device(best_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 3, D3D11_SDK_VERSION,
                    &device, &level, &context);
  safe_release(best_adapter);
  if (FAILED(hr) || !device || !context) {
    safe_release(output1);
    safe_release(factory);
    return false;
  }

  IDXGIOutputDuplication* dup = nullptr;
  hr = output1->DuplicateOutput(device, &dup);
  if (FAILED(hr) || !dup) {
    safe_release(context);
    safe_release(device);
    safe_release(output1);
    safe_release(factory);
    return false;
  }

  D3D11_TEXTURE2D_DESC td{};
  td.Width = static_cast<UINT>(w);
  td.Height = static_cast<UINT>(h);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* staging = nullptr;
  hr = device->CreateTexture2D(&td, nullptr, &staging);
  if (FAILED(hr) || !staging) {
    safe_release(dup);
    safe_release(context);
    safe_release(device);
    safe_release(output1);
    safe_release(factory);
    return false;
  }

  factory_ = factory;
  device_ = device;
  context_ = context;
  output1_ = output1;
  dup_ = dup;
  staging_ = staging;
  width_ = w;
  height_ = h;
  have_frame_ = false;
  frame_.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
  return true;
}

void DxgiCapture::release_duplication() {
  if (staging_) {
    static_cast<ID3D11Texture2D*>(staging_)->Release();
    staging_ = nullptr;
  }
  if (dup_) {
    static_cast<IDXGIOutputDuplication*>(dup_)->Release();
    dup_ = nullptr;
  }
  if (output1_) {
    static_cast<IDXGIOutput1*>(output1_)->Release();
    output1_ = nullptr;
  }
  if (context_) {
    static_cast<ID3D11DeviceContext*>(context_)->Release();
    context_ = nullptr;
  }
  if (device_) {
    static_cast<ID3D11Device*>(device_)->Release();
    device_ = nullptr;
  }
  if (factory_) {
    static_cast<IDXGIFactory1*>(factory_)->Release();
    factory_ = nullptr;
  }
  width_ = 0;
  height_ = 0;
  have_frame_ = false;
  frame_.clear();
  scratch_.clear();
  move_meta_.clear();
  dirty_meta_.clear();
}

bool DxgiCapture::reinit() {
  release_duplication();
  return create_duplication();
}

bool DxgiCapture::begin() {
  end();
  if (!create_duplication()) {
    unload_apis();
    return false;
  }
  return true;
}

void DxgiCapture::end() {
  release_duplication();
  unload_apis();
}

bool DxgiCapture::capture(std::vector<uint8_t>* bgra, int* width, int* height,
                          std::vector<CaptureDirty>* dirties, std::vector<CaptureMove>* moves,
                          bool force_full_pixels) {
  if (!bgra || !width || !height || !dirties || !moves || !dup_ || !staging_ || !context_) {
    return false;
  }
  dirties->clear();
  moves->clear();

  auto* dup = static_cast<IDXGIOutputDuplication*>(dup_);
  auto* ctx = static_cast<ID3D11DeviceContext*>(context_);
  auto* staging = static_cast<ID3D11Texture2D*>(staging_);

  DXGI_OUTDUPL_FRAME_INFO info{};
  IDXGIResource* resource = nullptr;
  HRESULT hr = dup->AcquireNextFrame(0, &info, &resource);
  if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
    *width = width_;
    *height = height_;
    bgra->assign(frame_.begin(), frame_.end());
    return true;  // idle: empty dirties/moves
  }
  if (hr == DXGI_ERROR_ACCESS_LOST) {
    safe_release(resource);
    if (!reinit()) {
      return false;
    }
    dup = static_cast<IDXGIOutputDuplication*>(dup_);
    ctx = static_cast<ID3D11DeviceContext*>(context_);
    staging = static_cast<ID3D11Texture2D*>(staging_);
    hr = dup->AcquireNextFrame(100, &info, &resource);
    if (FAILED(hr)) {
      if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        *width = width_;
        *height = height_;
        bgra->assign(frame_.begin(), frame_.end());
        return true;
      }
      return false;
    }
  }
  if (FAILED(hr) || !resource) {
    safe_release(resource);
    return false;
  }

  ID3D11Texture2D* tex = nullptr;
  hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
  safe_release(resource);
  if (FAILED(hr) || !tex) {
    dup->ReleaseFrame();
    return false;
  }

  ctx->CopyResource(staging, tex);
  safe_release(tex);

  bool need_full = !have_frame_ || force_full_pixels;
  std::vector<CaptureMove> pending_moves;
  std::vector<CaptureDirty> pending_dirties;
  if (!need_full && info.TotalMetadataBufferSize > 0) {
    // Separate buffers — some builds mishandle split-buffer Move+Dirty packing.
    move_meta_.resize(info.TotalMetadataBufferSize);
    UINT move_bytes = info.TotalMetadataBufferSize;
    hr = dup->GetFrameMoveRects(move_bytes,
                                reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(move_meta_.data()),
                                &move_bytes);
    if (SUCCEEDED(hr) && move_bytes >= sizeof(DXGI_OUTDUPL_MOVE_RECT)) {
      const auto* mlist = reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(move_meta_.data());
      const UINT move_count = move_bytes / sizeof(DXGI_OUTDUPL_MOVE_RECT);
      for (UINT i = 0; i < move_count; ++i) {
        CaptureMove mv{};
        if (move_to_capture(mlist[i], width_, height_, &mv)) {
          pending_moves.push_back(mv);
        }
        // Skip OOB moves; do not force full frame (that discarded all CopyRects).
      }
    }

    dirty_meta_.resize(info.TotalMetadataBufferSize);
    UINT dirty_bytes = info.TotalMetadataBufferSize;
    hr = dup->GetFrameDirtyRects(dirty_bytes, reinterpret_cast<RECT*>(dirty_meta_.data()),
                                 &dirty_bytes);
    if (SUCCEEDED(hr) && dirty_bytes >= sizeof(RECT)) {
      const auto* rects = reinterpret_cast<const RECT*>(dirty_meta_.data());
      const UINT dirty_count = dirty_bytes / sizeof(RECT);
      for (UINT i = 0; i < dirty_count; ++i) {
        CaptureDirty d{};
        if (!rect_to_dirty(rects[i], width_, height_, &d)) {
          continue;
        }
        if (d.x == 0 && d.y == 0 && d.w == width_ && d.h == height_) {
          // Full dirty alone → keyframe. If MoveRects exist, keep them (VNC-style) and
          // skip adding the full rect so CopyRect is not discarded.
          if (pending_moves.empty()) {
            need_full = true;
            pending_dirties.clear();
            break;
          }
          continue;
        }
        pending_dirties.push_back(d);
      }
    } else if (pending_moves.empty()) {
      // No metadata usable — treat as full refresh of pixels only when brand new.
      if (!have_frame_) {
        need_full = true;
      }
    }
  } else if (!have_frame_) {
    need_full = true;
  }

  D3D11_MAPPED_SUBRESOURCE mapped{};
  hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    dup->ReleaseFrame();
    return false;
  }

  const size_t row_bytes = static_cast<size_t>(width_) * 4u;
  const auto* src = static_cast<const uint8_t*>(mapped.pData);
  if (need_full || force_full_pixels) {
    for (int y = 0; y < height_; ++y) {
      std::memcpy(frame_.data() + static_cast<size_t>(y) * row_bytes,
                  src + static_cast<size_t>(y) * mapped.RowPitch, row_bytes);
    }
  } else {
    if (!pending_moves.empty()) {
      scratch_ = frame_;
      for (const CaptureMove& mv : pending_moves) {
        for (int row = 0; row < mv.h; ++row) {
          const uint8_t* srow =
              frame_.data() + (static_cast<size_t>(mv.sy + row) * width_ + mv.sx) * 4u;
          uint8_t* drow =
              scratch_.data() + (static_cast<size_t>(mv.dy + row) * width_ + mv.dx) * 4u;
          std::memcpy(drow, srow, static_cast<size_t>(mv.w) * 4u);
        }
      }
      frame_.swap(scratch_);
    }
    for (const CaptureDirty& d : pending_dirties) {
      for (int row = 0; row < d.h; ++row) {
        const size_t dy = static_cast<size_t>(d.y + row);
        std::memcpy(frame_.data() + (dy * width_ + static_cast<size_t>(d.x)) * 4u,
                    src + dy * mapped.RowPitch + static_cast<size_t>(d.x) * 4u,
                    static_cast<size_t>(d.w) * 4u);
      }
    }
  }
  ctx->Unmap(staging, 0);
  dup->ReleaseFrame();

  if (force_full_pixels) {
    dirties->clear();
    moves->clear();
  } else if (need_full) {
    dirties->clear();
    moves->clear();
    dirties->push_back(CaptureDirty{0, 0, width_, height_});
  } else {
    *moves = std::move(pending_moves);
    *dirties = std::move(pending_dirties);
  }

  bgra->assign(frame_.begin(), frame_.end());
  *width = width_;
  *height = height_;
  have_frame_ = true;
  return true;
}

}  // namespace road_desk::replace
