#include "jpeg_encode.h"

#include "mux_protocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <wincodec.h>

#include <cstring>

namespace road_desk::media {
namespace {

using road_desk::replace::kMaxPayloadLen;
using road_desk::replace::kVideoHeaderSize;
using road_desk::replace::kVideoJpeg;

void write_u16_le(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

void write_u32_le(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

bool rect_in_desk(int desk_w, int desk_h, int x, int y, int rw, int rh) {
  if (desk_w <= 0 || desk_h <= 0 || rw <= 0 || rh <= 0) {
    return false;
  }
  if (x < 0 || y < 0) {
    return false;
  }
  if (x > desk_w - rw || y > desk_h - rh) {
    return false;
  }
  return true;
}

struct ComScope {
  bool uninit = false;
  bool ok = false;
  ComScope() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == S_OK) {
      uninit = true;
      ok = true;
    } else if (hr == S_FALSE) {
      ok = true;
    }
  }
  ~ComScope() {
    if (uninit) {
      CoUninitialize();
    }
  }
};

template <typename T>
void safe_release(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

}  // namespace

bool encode_jpeg_rect(uint32_t frame_id, int desk_w, int desk_h, int x, int y, int rw, int rh,
                      const uint8_t* bgra_full, int quality, std::vector<uint8_t>* raw_buf,
                      std::vector<uint8_t>* body_buf, EncodeRectStats* st) {
  if (!bgra_full || !raw_buf || !body_buf || !rect_in_desk(desk_w, desk_h, x, y, rw, rh)) {
    return false;
  }
  if (quality < 1) {
    quality = 1;
  }
  if (quality > 100) {
    quality = 100;
  }
  const uint32_t pix = static_cast<uint32_t>(rw) * static_cast<uint32_t>(rh) * 4u;
  if (pix == 0 || static_cast<uint32_t>(kVideoHeaderSize) + pix > kMaxPayloadLen) {
    return false;
  }

  raw_buf->resize(pix);
  uint8_t* raw = raw_buf->data();
  for (int row = 0; row < rh; ++row) {
    const uint8_t* src = bgra_full + (static_cast<size_t>(y + row) * desk_w + x) * 4u;
    std::memcpy(raw + static_cast<size_t>(row) * rw * 4u, src, static_cast<size_t>(rw) * 4u);
  }

  ComScope com;
  if (!com.ok) {
    return false;
  }
  IWICImagingFactory* factory = nullptr;
  // Prefer WIC1: Win8+ SDKs alias CLSID_WICImagingFactory to Factory2 (missing on Win7).
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  if (FAILED(hr) || !factory) {
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&factory));
  }
  if (FAILED(hr) || !factory) {
    return false;
  }

  IWICBitmap* bitmap = nullptr;
  hr = factory->CreateBitmapFromMemory(static_cast<UINT>(rw), static_cast<UINT>(rh),
                                       GUID_WICPixelFormat32bppBGRA, static_cast<UINT>(rw) * 4u,
                                       pix, raw, &bitmap);
  if (FAILED(hr) || !bitmap) {
    safe_release(factory);
    return false;
  }

  IStream* stream = nullptr;
  hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
  if (FAILED(hr) || !stream) {
    safe_release(bitmap);
    safe_release(factory);
    return false;
  }

  IWICBitmapEncoder* encoder = nullptr;
  hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
  if (FAILED(hr) || !encoder) {
    safe_release(stream);
    safe_release(bitmap);
    safe_release(factory);
    return false;
  }
  hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
  if (FAILED(hr)) {
    safe_release(encoder);
    safe_release(stream);
    safe_release(bitmap);
    safe_release(factory);
    return false;
  }

  IWICBitmapFrameEncode* frame = nullptr;
  IPropertyBag2* props = nullptr;
  hr = encoder->CreateNewFrame(&frame, &props);
  if (FAILED(hr) || !frame) {
    safe_release(props);
    safe_release(encoder);
    safe_release(stream);
    safe_release(bitmap);
    safe_release(factory);
    return false;
  }

  if (props) {
    PROPBAG2 opt{};
    opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_R4;
    var.fltVal = static_cast<float>(quality) / 100.f;
    props->Write(1, &opt, &var);
    VariantClear(&var);
  }

  hr = frame->Initialize(props);
  safe_release(props);
  if (FAILED(hr)) {
    safe_release(frame);
    safe_release(encoder);
    safe_release(stream);
    safe_release(bitmap);
    safe_release(factory);
    return false;
  }
  hr = frame->SetSize(static_cast<UINT>(rw), static_cast<UINT>(rh));
  if (FAILED(hr)) {
    safe_release(frame);
    safe_release(encoder);
    safe_release(stream);
    safe_release(bitmap);
    safe_release(factory);
    return false;
  }
  WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
  hr = frame->SetPixelFormat(&fmt);
  if (FAILED(hr)) {
    safe_release(frame);
    safe_release(encoder);
    safe_release(stream);
    safe_release(bitmap);
    safe_release(factory);
    return false;
  }
  hr = frame->WriteSource(bitmap, nullptr);
  if (FAILED(hr)) {
    safe_release(frame);
    safe_release(encoder);
    safe_release(stream);
    safe_release(bitmap);
    safe_release(factory);
    return false;
  }
  hr = frame->Commit();
  safe_release(frame);
  if (FAILED(hr)) {
    safe_release(encoder);
    safe_release(stream);
    safe_release(bitmap);
    safe_release(factory);
    return false;
  }
  hr = encoder->Commit();
  safe_release(encoder);
  safe_release(bitmap);
  safe_release(factory);
  if (FAILED(hr)) {
    safe_release(stream);
    return false;
  }

  HGLOBAL hg = nullptr;
  if (FAILED(GetHGlobalFromStream(stream, &hg)) || !hg) {
    safe_release(stream);
    return false;
  }
  const SIZE_T jpeg_len = GlobalSize(hg);
  const void* jpeg_ptr = GlobalLock(hg);
  if (!jpeg_ptr || jpeg_len == 0 ||
      static_cast<uint32_t>(kVideoHeaderSize) + static_cast<uint32_t>(jpeg_len) > kMaxPayloadLen) {
    if (jpeg_ptr) {
      GlobalUnlock(hg);
    }
    safe_release(stream);
    return false;
  }

  body_buf->resize(static_cast<size_t>(kVideoHeaderSize) + static_cast<size_t>(jpeg_len));
  uint8_t* body = body_buf->data();
  body[0] = kVideoJpeg;
  write_u32_le(body + 1, frame_id);
  write_u16_le(body + 5, static_cast<uint16_t>(x));
  write_u16_le(body + 7, static_cast<uint16_t>(y));
  write_u16_le(body + 9, static_cast<uint16_t>(rw));
  write_u16_le(body + 11, static_cast<uint16_t>(rh));
  std::memcpy(body + kVideoHeaderSize, jpeg_ptr, static_cast<size_t>(jpeg_len));
  GlobalUnlock(hg);
  safe_release(stream);

  if (st) {
    st->raw_bytes = pix;
    st->wire_bytes = static_cast<uint32_t>(body_buf->size());
    st->codec = kVideoJpeg;
  }
  return true;
}

bool decode_jpeg_to_bgra(const uint8_t* jpeg, size_t jpeg_len, int expect_w, int expect_h,
                         std::vector<uint8_t>* bgra_out) {
  if (!jpeg || !bgra_out || jpeg_len == 0 || expect_w <= 0 || expect_h <= 0) {
    return false;
  }
  ComScope com;
  if (!com.ok) {
    return false;
  }
  IWICImagingFactory* factory = nullptr;
  // Prefer WIC1: Win8+ SDKs alias CLSID_WICImagingFactory to Factory2 (missing on Win7).
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  if (FAILED(hr) || !factory) {
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&factory));
  }
  if (FAILED(hr) || !factory) {
    return false;
  }

  IWICStream* stream = nullptr;
  hr = factory->CreateStream(&stream);
  if (FAILED(hr) || !stream) {
    safe_release(factory);
    return false;
  }
  hr = stream->InitializeFromMemory(const_cast<BYTE*>(reinterpret_cast<const BYTE*>(jpeg)),
                                    static_cast<DWORD>(jpeg_len));
  if (FAILED(hr)) {
    safe_release(stream);
    safe_release(factory);
    return false;
  }

  IWICBitmapDecoder* decoder = nullptr;
  hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
  safe_release(stream);
  if (FAILED(hr) || !decoder) {
    safe_release(factory);
    return false;
  }

  IWICBitmapFrameDecode* frame = nullptr;
  hr = decoder->GetFrame(0, &frame);
  safe_release(decoder);
  if (FAILED(hr) || !frame) {
    safe_release(factory);
    return false;
  }

  UINT w = 0;
  UINT h = 0;
  frame->GetSize(&w, &h);
  if (static_cast<int>(w) != expect_w || static_cast<int>(h) != expect_h) {
    safe_release(frame);
    safe_release(factory);
    return false;
  }

  IWICFormatConverter* conv = nullptr;
  hr = factory->CreateFormatConverter(&conv);
  if (FAILED(hr) || !conv) {
    safe_release(frame);
    safe_release(factory);
    return false;
  }
  hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                        WICBitmapPaletteTypeCustom);
  safe_release(frame);
  if (FAILED(hr)) {
    safe_release(conv);
    safe_release(factory);
    return false;
  }

  const size_t out_bytes = static_cast<size_t>(expect_w) * static_cast<size_t>(expect_h) * 4u;
  bgra_out->resize(out_bytes);
  hr = conv->CopyPixels(nullptr, static_cast<UINT>(expect_w) * 4u, static_cast<UINT>(out_bytes),
                        bgra_out->data());
  safe_release(conv);
  safe_release(factory);
  return SUCCEEDED(hr);
}

}  // namespace road_desk::media
