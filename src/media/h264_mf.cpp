#include "h264_mf.h"

#include "mux_protocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <mferror.h>
#include <objbase.h>
#include <oleauto.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace road_desk::media {
namespace {

using road_desk::replace::kMaxPayloadLen;
using road_desk::replace::kVideoHeaderSize;
using road_desk::replace::kVideoH264;

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

template <typename T>
void safe_release(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

struct MfOnce {
  bool ok = false;
  bool com = false;
  MfOnce() {
    const HRESULT chr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com = (chr == S_OK);
    if (FAILED(chr) && chr != S_FALSE && chr != RPC_E_CHANGED_MODE) {
      return;
    }
    const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    ok = SUCCEEDED(hr);
  }
  ~MfOnce() {
    if (ok) {
      MFShutdown();
    }
    if (com) {
      CoUninitialize();
    }
  }
};

MfOnce& mf_once() {
  static MfOnce g;
  return g;
}

int nv12_stride(int w) {
  return (w + 15) & ~15;
}

size_t nv12_plane_bytes(int /*w*/, int h, int stride) {
  return static_cast<size_t>(stride) * static_cast<size_t>(h) +
         static_cast<size_t>(stride) * static_cast<size_t>(h / 2);
}

void bgra_rect_to_nv12(const uint8_t* bgra, int desk_w, int x, int y, int w, int h, int stride,
                       uint8_t* nv12) {
  uint8_t* yplane = nv12;
  uint8_t* uv = nv12 + static_cast<size_t>(stride) * static_cast<size_t>(h);
  for (int row = 0; row < h; ++row) {
    const uint8_t* src = bgra + (static_cast<size_t>(y + row) * desk_w + x) * 4u;
    uint8_t* yd = yplane + static_cast<size_t>(row) * stride;
    for (int col = 0; col < w; ++col) {
      const int B = src[col * 4 + 0];
      const int G = src[col * 4 + 1];
      const int R = src[col * 4 + 2];
      int Y = ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16;
      if (Y < 0) {
        Y = 0;
      }
      if (Y > 255) {
        Y = 255;
      }
      yd[col] = static_cast<uint8_t>(Y);
    }
    if (stride > w) {
      std::memset(yd + w, 0, static_cast<size_t>(stride - w));
    }
  }
  for (int row = 0; row < h; row += 2) {
    const uint8_t* s0 = bgra + (static_cast<size_t>(y + row) * desk_w + x) * 4u;
    const uint8_t* s1 =
        bgra + (static_cast<size_t>(y + (std::min)(row + 1, h - 1)) * desk_w + x) * 4u;
    uint8_t* uvd = uv + static_cast<size_t>(row / 2) * stride;
    for (int col = 0; col < w; col += 2) {
      const int c1 = col;
      const int c2 = (std::min)(col + 1, w - 1);
      const int B = (s0[c1 * 4] + s0[c2 * 4] + s1[c1 * 4] + s1[c2 * 4]) / 4;
      const int G = (s0[c1 * 4 + 1] + s0[c2 * 4 + 1] + s1[c1 * 4 + 1] + s1[c2 * 4 + 1]) / 4;
      const int R = (s0[c1 * 4 + 2] + s0[c2 * 4 + 2] + s1[c1 * 4 + 2] + s1[c2 * 4 + 2]) / 4;
      int U = ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
      int V = ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;
      if (U < 0) {
        U = 0;
      }
      if (U > 255) {
        U = 255;
      }
      if (V < 0) {
        V = 0;
      }
      if (V > 255) {
        V = 255;
      }
      uvd[col] = static_cast<uint8_t>(U);
      uvd[col + 1] = static_cast<uint8_t>(V);
    }
    if (stride > w) {
      std::memset(uvd + w, 0, static_cast<size_t>(stride - w));
    }
  }
}

void nv12_to_bgra_strided(const uint8_t* nv12, int w, int h, LONG y_pitch, uint8_t* bgra) {
  const uint8_t* yplane = nv12;
  const uint8_t* uv = nv12 + static_cast<size_t>(y_pitch) * h;
  for (int row = 0; row < h; ++row) {
    const uint8_t* yp = yplane + static_cast<size_t>(row) * y_pitch;
    const uint8_t* uvp = uv + static_cast<size_t>(row / 2) * y_pitch;
    uint8_t* dst = bgra + static_cast<size_t>(row) * w * 4u;
    for (int col = 0; col < w; ++col) {
      const int Y = static_cast<int>(yp[col]) - 16;
      const int U = static_cast<int>(uvp[col & ~1]) - 128;
      const int V = static_cast<int>(uvp[(col & ~1) + 1]) - 128;
      int R = (298 * Y + 409 * V + 128) >> 8;
      int G = (298 * Y - 100 * U - 208 * V + 128) >> 8;
      int B = (298 * Y + 516 * U + 128) >> 8;
      if (R < 0) {
        R = 0;
      }
      if (R > 255) {
        R = 255;
      }
      if (G < 0) {
        G = 0;
      }
      if (G > 255) {
        G = 255;
      }
      if (B < 0) {
        B = 0;
      }
      if (B > 255) {
        B = 255;
      }
      dst[col * 4 + 0] = static_cast<uint8_t>(B);
      dst[col * 4 + 1] = static_cast<uint8_t>(G);
      dst[col * 4 + 2] = static_cast<uint8_t>(R);
      dst[col * 4 + 3] = 255;
    }
  }
}

struct EncState {
  IMFTransform* xf = nullptr;
  int w = 0;
  int h = 0;
  int stride = 0;
  LONGLONG ts = 0;
};

struct DecState {
  IMFTransform* xf = nullptr;
  int w = 0;
  int h = 0;
  int stride = 0;
};

EncState g_enc;
DecState g_dec;
CRITICAL_SECTION g_enc_cs;
CRITICAL_SECTION g_dec_cs;
INIT_ONCE g_cs_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK init_h264_cs_once(PINIT_ONCE, PVOID, PVOID*) {
  InitializeCriticalSection(&g_enc_cs);
  InitializeCriticalSection(&g_dec_cs);
  return TRUE;
}

void ensure_cs() {
  InitOnceExecuteOnce(&g_cs_once, init_h264_cs_once, nullptr, nullptr);
}

struct CsLock {
  CRITICAL_SECTION* cs;
  explicit CsLock(CRITICAL_SECTION* c) : cs(c) { EnterCriticalSection(cs); }
  ~CsLock() { LeaveCriticalSection(cs); }
  CsLock(const CsLock&) = delete;
  CsLock& operator=(const CsLock&) = delete;
};

DWORD nv12_buffer_bytes(int w, int h) {
  const int stride = nv12_stride(w);
  return static_cast<DWORD>(nv12_plane_bytes(w, h, stride)) + 8192u;
}

int stride_from_type(IMFMediaType* type, int w) {
  UINT32 s = 0;
  if (type && SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) && s > 0) {
    return static_cast<int>(s);
  }
  LONG pitch = 0;
  // NV12 FOURCC = 0x3231564E
  if (SUCCEEDED(MFGetStrideForBitmapInfoHeader(0x3231564Eu, static_cast<DWORD>(w), &pitch)) &&
      pitch != 0) {
    return pitch > 0 ? static_cast<int>(pitch) : static_cast<int>(-pitch);
  }
  return nv12_stride(w);
}

// Enumerate and force NV12. Updates *out_w/*out_h/*out_stride from the chosen type when set.
bool apply_decoder_output_nv12(IMFTransform* xf, int expect_w, int expect_h, int* out_w,
                               int* out_h, int* out_stride) {
  for (DWORD i = 0;; ++i) {
    IMFMediaType* cand = nullptr;
    const HRESULT enum_hr = xf->GetOutputAvailableType(0, i, &cand);
    if (FAILED(enum_hr) || !cand) {
      break;
    }
    GUID subtype{};
    const bool is_nv12 =
        SUCCEEDED(cand->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12;
    if (!is_nv12) {
      safe_release(cand);
      continue;
    }
    // Always pin our rect geometry (independent-IDR protocol).
    MFSetAttributeSize(cand, MF_MT_FRAME_SIZE, static_cast<UINT32>(expect_w),
                       static_cast<UINT32>(expect_h));
    const int stride = stride_from_type(cand, expect_w);
    cand->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(stride));
    const HRESULT hr = xf->SetOutputType(0, cand, 0);
    safe_release(cand);
    if (FAILED(hr)) {
      continue;
    }
    if (out_w) {
      *out_w = expect_w;
    }
    if (out_h) {
      *out_h = expect_h;
    }
    if (out_stride) {
      *out_stride = stride;
    }
    return true;
  }

  // Fallback: request NV12 explicitly.
  IMFMediaType* out_type = nullptr;
  MFCreateMediaType(&out_type);
  out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(out_type, MF_MT_FRAME_SIZE, static_cast<UINT32>(expect_w),
                     static_cast<UINT32>(expect_h));
  MFSetAttributeRatio(out_type, MF_MT_FRAME_RATE, 30, 1);
  MFSetAttributeRatio(out_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  const int stride = nv12_stride(expect_w);
  out_type->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(stride));
  const HRESULT hr = xf->SetOutputType(0, out_type, 0);
  safe_release(out_type);
  if (FAILED(hr)) {
    return false;
  }
  if (out_w) {
    *out_w = expect_w;
  }
  if (out_h) {
    *out_h = expect_h;
  }
  if (out_stride) {
    *out_stride = stride;
  }
  return true;
}

bool create_h264_encoder(int w, int h, int stride, int quality, IMFTransform** out) {
  *out = nullptr;
  IMFTransform* xf = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&xf));
  if (FAILED(hr) || !xf) {
    return false;
  }

  ICodecAPI* api = nullptr;
  if (SUCCEEDED(xf->QueryInterface(IID_PPV_ARGS(&api))) && api) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = eAVEncCommonRateControlMode_Quality;
    api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
    v.ulVal = static_cast<ULONG>((std::max)(1, (std::min)(100, quality)));
    api->SetValue(&CODECAPI_AVEncCommonQuality, &v);
    v.vt = VT_BOOL;
    v.boolVal = VARIANT_TRUE;
    api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    VariantClear(&v);
    safe_release(api);
  }

  IMFMediaType* out_type = nullptr;
  MFCreateMediaType(&out_type);
  out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  MFSetAttributeSize(out_type, MF_MT_FRAME_SIZE, static_cast<UINT32>(w), static_cast<UINT32>(h));
  MFSetAttributeRatio(out_type, MF_MT_FRAME_RATE, 30, 1);
  MFSetAttributeRatio(out_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  out_type->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(w * h * 4));
  out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
  hr = xf->SetOutputType(0, out_type, 0);
  safe_release(out_type);
  if (FAILED(hr)) {
    safe_release(xf);
    return false;
  }

  IMFMediaType* in_type = nullptr;
  MFCreateMediaType(&in_type);
  in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(in_type, MF_MT_FRAME_SIZE, static_cast<UINT32>(w), static_cast<UINT32>(h));
  MFSetAttributeRatio(in_type, MF_MT_FRAME_RATE, 30, 1);
  MFSetAttributeRatio(in_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  in_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  in_type->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(stride));
  hr = xf->SetInputType(0, in_type, 0);
  safe_release(in_type);
  if (FAILED(hr)) {
    safe_release(xf);
    return false;
  }

  xf->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  xf->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  xf->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  *out = xf;
  return true;
}

bool create_h264_decoder(int w, int h, int* out_stride, IMFTransform** out) {
  *out = nullptr;
  if (out_stride) {
    *out_stride = nv12_stride(w);
  }
  IMFTransform* xf = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&xf));
  if (FAILED(hr) || !xf) {
    return false;
  }

  IMFMediaType* in_type = nullptr;
  MFCreateMediaType(&in_type);
  in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  MFSetAttributeSize(in_type, MF_MT_FRAME_SIZE, static_cast<UINT32>(w), static_cast<UINT32>(h));
  MFSetAttributeRatio(in_type, MF_MT_FRAME_RATE, 30, 1);
  MFSetAttributeRatio(in_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  hr = xf->SetInputType(0, in_type, 0);
  safe_release(in_type);
  if (FAILED(hr)) {
    safe_release(xf);
    return false;
  }

  int dw = w;
  int dh = h;
  int stride = nv12_stride(w);
  if (!apply_decoder_output_nv12(xf, w, h, &dw, &dh, &stride)) {
    safe_release(xf);
    return false;
  }
  // Placeholder output types may advertise a different size until STREAM_CHANGE;
  // we always request expect geometry in apply_decoder_output_nv12.

  xf->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  xf->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  xf->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  if (out_stride) {
    *out_stride = stride;
  }
  *out = xf;
  return true;
}

bool ensure_encoder(int w, int h, int stride, int quality) {
  if (!mf_once().ok) {
    return false;
  }
  ensure_cs();
  if (g_enc.xf && g_enc.w == w && g_enc.h == h && g_enc.stride == stride) {
    return true;
  }
  safe_release(g_enc.xf);
  g_enc = {};
  if (!create_h264_encoder(w, h, stride, quality, &g_enc.xf)) {
    return false;
  }
  g_enc.w = w;
  g_enc.h = h;
  g_enc.stride = stride;
  g_enc.ts = 0;
  return true;
}

bool ensure_decoder(int w, int h) {
  if (!mf_once().ok) {
    return false;
  }
  ensure_cs();
  if (g_dec.xf && g_dec.w == w && g_dec.h == h) {
    return true;
  }
  safe_release(g_dec.xf);
  g_dec = {};
  int stride = 0;
  if (!create_h264_decoder(w, h, &stride, &g_dec.xf)) {
    return false;
  }
  g_dec.w = w;
  g_dec.h = h;
  g_dec.stride = stride;
  return true;
}

bool mft_pull_bitstream(IMFTransform* xf, std::vector<uint8_t>* out) {
  out->clear();
  // Hard cap: a wedged MFT returning S_OK forever used to hang the mux thread.
  for (int n = 0; n < 64; ++n) {
    MFT_OUTPUT_STREAM_INFO info{};
    xf->GetOutputStreamInfo(0, &info);
    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    const DWORD need = (std::max)(info.cbSize, static_cast<DWORD>(64u * 1024u));
    if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(need, &buffer))) {
      safe_release(buffer);
      safe_release(sample);
      return false;
    }
    sample->AddBuffer(buffer);
    safe_release(buffer);

    MFT_OUTPUT_DATA_BUFFER odb{};
    odb.pSample = sample;
    DWORD status = 0;
    const HRESULT hr = xf->ProcessOutput(0, 1, &odb, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      safe_release(sample);
      return !out->empty();
    }
    if (FAILED(hr)) {
      safe_release(sample);
      return !out->empty();
    }
    IMFMediaBuffer* outb = nullptr;
    sample->ConvertToContiguousBuffer(&outb);
    if (outb) {
      BYTE* data = nullptr;
      DWORD max_len = 0;
      DWORD cur_len = 0;
      if (SUCCEEDED(outb->Lock(&data, &max_len, &cur_len)) && data && cur_len > 0) {
        const size_t old = out->size();
        out->resize(old + cur_len);
        std::memcpy(out->data() + old, data, cur_len);
        outb->Unlock();
      }
      safe_release(outb);
    }
    safe_release(sample);
  }
  return !out->empty();
}

}  // namespace

bool encode_h264_rect(uint32_t frame_id, int desk_w, int desk_h, int x, int y, int rw, int rh,
                      const uint8_t* bgra_full, int quality, std::vector<uint8_t>* scratch_nv12,
                      std::vector<uint8_t>* body_buf, EncodeRectStats* st) {
  if (!bgra_full || !scratch_nv12 || !body_buf || !rect_in_desk(desk_w, desk_h, x, y, rw, rh)) {
    return false;
  }
  // H.264 needs 16x16 macroblocks. Expand (don't crop) so dirty rims aren't left as ghosts.
  int ew = (rw + 15) & ~15;
  int eh = (rh + 15) & ~15;
  if (ew < 16 || eh < 16) {
    return false;
  }
  if (x + ew > desk_w) {
    x = desk_w - ew;
    if (x < 0) {
      x = 0;
      ew = desk_w & ~15;
    }
  }
  if (y + eh > desk_h) {
    y = desk_h - eh;
    if (y < 0) {
      y = 0;
      eh = desk_h & ~15;
    }
  }
  if (ew < 16 || eh < 16 || !rect_in_desk(desk_w, desk_h, x, y, ew, eh)) {
    return false;
  }
  rw = ew;
  rh = eh;
  const int stride = nv12_stride(rw);
  ensure_cs();
  CsLock lock(&g_enc_cs);
  if (!ensure_encoder(rw, rh, stride, quality)) {
    return false;
  }

  const size_t nv12_bytes = nv12_plane_bytes(rw, rh, stride);
  scratch_nv12->assign(nv12_bytes, 0);
  bgra_rect_to_nv12(bgra_full, desk_w, x, y, rw, rh, stride, scratch_nv12->data());

  ICodecAPI* api = nullptr;
  if (SUCCEEDED(g_enc.xf->QueryInterface(IID_PPV_ARGS(&api))) && api) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = 1;
    api->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
    VariantClear(&v);
    safe_release(api);
  }

  IMFMediaBuffer* buffer = nullptr;
  IMFSample* sample = nullptr;
  HRESULT hr = MFCreateAlignedMemoryBuffer(static_cast<DWORD>(nv12_bytes), 16, &buffer);
  if (FAILED(hr)) {
    hr = MFCreateMemoryBuffer(static_cast<DWORD>(nv12_bytes), &buffer);
  }
  if (FAILED(hr) || FAILED(MFCreateSample(&sample))) {
    safe_release(buffer);
    safe_release(sample);
    return false;
  }
  BYTE* dst = nullptr;
  DWORD max_len = 0;
  if (FAILED(buffer->Lock(&dst, &max_len, nullptr)) || !dst || max_len < nv12_bytes) {
    safe_release(buffer);
    safe_release(sample);
    return false;
  }
  std::memcpy(dst, scratch_nv12->data(), nv12_bytes);
  buffer->Unlock();
  buffer->SetCurrentLength(static_cast<DWORD>(nv12_bytes));
  sample->AddBuffer(buffer);
  safe_release(buffer);
  sample->SetSampleTime(g_enc.ts);
  sample->SetSampleDuration(333333);
  g_enc.ts += 333333;

  hr = g_enc.xf->ProcessInput(0, sample, 0);
  safe_release(sample);
  if (FAILED(hr)) {
    return false;
  }

  std::vector<uint8_t> bitstream;
  if (!mft_pull_bitstream(g_enc.xf, &bitstream) || bitstream.empty()) {
    return false;
  }
  if (static_cast<uint32_t>(kVideoHeaderSize) + bitstream.size() > kMaxPayloadLen) {
    return false;
  }

  body_buf->resize(static_cast<size_t>(kVideoHeaderSize) + bitstream.size());
  uint8_t* body = body_buf->data();
  body[0] = kVideoH264;
  write_u32_le(body + 1, frame_id);
  write_u16_le(body + 5, static_cast<uint16_t>(x));
  write_u16_le(body + 7, static_cast<uint16_t>(y));
  write_u16_le(body + 9, static_cast<uint16_t>(rw));
  write_u16_le(body + 11, static_cast<uint16_t>(rh));
  std::memcpy(body + kVideoHeaderSize, bitstream.data(), bitstream.size());
  if (st) {
    st->raw_bytes = static_cast<uint32_t>(rw) * rh * 4u;
    st->wire_bytes = static_cast<uint32_t>(body_buf->size());
    st->codec = kVideoH264;
  }
  return true;
}

static bool decode_h264_to_bgra_unlocked(const uint8_t* annex_b, size_t len, int expect_w,
                                         int expect_h, std::vector<uint8_t>* bgra_out) {
  IMFMediaBuffer* buffer = nullptr;
  IMFSample* sample = nullptr;
  if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(len), &buffer)) ||
      FAILED(MFCreateSample(&sample))) {
    safe_release(buffer);
    safe_release(sample);
    return false;
  }
  BYTE* dst = nullptr;
  if (FAILED(buffer->Lock(&dst, nullptr, nullptr)) || !dst) {
    safe_release(buffer);
    safe_release(sample);
    return false;
  }
  std::memcpy(dst, annex_b, len);
  buffer->Unlock();
  buffer->SetCurrentLength(static_cast<DWORD>(len));
  sample->AddBuffer(buffer);
  safe_release(buffer);
  sample->SetSampleTime(0);
  sample->SetSampleDuration(333333);

  HRESULT hr = g_dec.xf->ProcessInput(0, sample, 0);
  safe_release(sample);
  if (FAILED(hr)) {
    return false;
  }

  for (int attempt = 0; attempt < 12; ++attempt) {
    MFT_OUTPUT_STREAM_INFO info{};
    g_dec.xf->GetOutputStreamInfo(0, &info);
    const bool provides = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    const bool can_provide = (info.dwFlags & MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES) != 0;

    // Prefer MFT-owned samples when allowed — undersized caller buffers AV in msmpeg2vdec.
    IMFSample* out_sample = nullptr;
    if (!provides && !can_provide) {
      DWORD need = info.cbSize;
      const DWORD guess = nv12_buffer_bytes(expect_w, expect_h);
      if (need < guess) {
        need = guess;
      }
      // Extra pad: some builds align coded height up to 16 internally.
      need += static_cast<DWORD>(nv12_stride(expect_w)) * 16u;
      IMFMediaBuffer* out_buf = nullptr;
      if (FAILED(MFCreateSample(&out_sample)) || FAILED(MFCreateMemoryBuffer(need, &out_buf))) {
        safe_release(out_buf);
        safe_release(out_sample);
        return false;
      }
      out_sample->AddBuffer(out_buf);
      safe_release(out_buf);
    }

    MFT_OUTPUT_DATA_BUFFER odb{};
    odb.pSample = out_sample;
    DWORD status = 0;
    hr = g_dec.xf->ProcessOutput(0, 1, &odb, &status);
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      safe_release(out_sample);
      if (odb.pSample && odb.pSample != out_sample) {
        safe_release(odb.pSample);
      }
      int dw = expect_w;
      int dh = expect_h;
      int stride = g_dec.stride;
      if (!apply_decoder_output_nv12(g_dec.xf, expect_w, expect_h, &dw, &dh, &stride)) {
        return false;
      }
      if (dw != expect_w || dh != expect_h) {
        return false;
      }
      g_dec.stride = stride;
      continue;
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      safe_release(out_sample);
      g_dec.xf->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
      continue;
    }
    if (FAILED(hr)) {
      safe_release(out_sample);
      if (odb.pSample && odb.pSample != out_sample) {
        safe_release(odb.pSample);
      }
      return false;
    }

    IMFSample* got = odb.pSample ? odb.pSample : out_sample;
    out_sample = nullptr;
    IMFMediaBuffer* contig = nullptr;
    if (got) {
      got->ConvertToContiguousBuffer(&contig);
      safe_release(got);
    }
    if (!contig) {
      return false;
    }

    LONG pitch = g_dec.stride > 0 ? g_dec.stride : nv12_stride(expect_w);
    BYTE* data = nullptr;
    DWORD cur = 0;
    IMF2DBuffer* buf2d = nullptr;
    if (SUCCEEDED(contig->QueryInterface(IID_PPV_ARGS(&buf2d))) && buf2d) {
      BYTE* scan = nullptr;
      if (FAILED(buf2d->Lock2D(&scan, &pitch)) || !scan || pitch < expect_w) {
        safe_release(buf2d);
        safe_release(contig);
        return false;
      }
      data = scan;
      bgra_out->resize(static_cast<size_t>(expect_w) * expect_h * 4u);
      nv12_to_bgra_strided(data, expect_w, expect_h, pitch, bgra_out->data());
      buf2d->Unlock2D();
      safe_release(buf2d);
      safe_release(contig);
      return true;
    }

    if (FAILED(contig->Lock(&data, nullptr, &cur)) || !data || cur == 0) {
      safe_release(contig);
      return false;
    }
    const size_t need_bytes = nv12_plane_bytes(expect_w, expect_h, pitch);
    if (cur < need_bytes) {
      const size_t tight = static_cast<size_t>(expect_w) * expect_h * 3u / 2u;
      if (cur < tight) {
        contig->Unlock();
        safe_release(contig);
        return false;
      }
      pitch = expect_w;
    }
    bgra_out->resize(static_cast<size_t>(expect_w) * expect_h * 4u);
    nv12_to_bgra_strided(data, expect_w, expect_h, pitch, bgra_out->data());
    contig->Unlock();
    safe_release(contig);
    return true;
  }
  return false;
}

static bool decode_h264_to_bgra_seh(const uint8_t* annex_b, size_t len, int expect_w, int expect_h,
                                    std::vector<uint8_t>* bgra_out) {
  bool ok = false;
  __try {
    ok = decode_h264_to_bgra_unlocked(annex_b, len, expect_w, expect_h, bgra_out);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // msmpeg2vdec AV must not kill Viewer; drop rect this frame.
    safe_release(g_dec.xf);
    g_dec = {};
    ok = false;
  }
  return ok;
}

bool decode_h264_to_bgra(const uint8_t* annex_b, size_t len, int expect_w, int expect_h,
                         std::vector<uint8_t>* bgra_out) {
  if (!annex_b || !bgra_out || len == 0 || expect_w < 16 || expect_h < 16) {
    return false;
  }
  expect_w &= ~15;
  expect_h &= ~15;
  if (expect_w < 16 || expect_h < 16) {
    return false;
  }
  ensure_cs();
  CsLock lock(&g_dec_cs);
  if (!ensure_decoder(expect_w, expect_h)) {
    return false;
  }
  return decode_h264_to_bgra_seh(annex_b, len, expect_w, expect_h, bgra_out);
}

void h264_codec_shutdown() {
  ensure_cs();
  {
    CsLock lock(&g_enc_cs);
    safe_release(g_enc.xf);
    g_enc = {};
  }
  {
    CsLock lock(&g_dec_cs);
    safe_release(g_dec.xf);
    g_dec = {};
  }
}

}  // namespace road_desk::media
