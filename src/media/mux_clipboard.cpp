#include "mux_clipboard.h"

#include "miniz.h"

#include <cstring>

namespace road_desk::replace {
namespace {

void write_u32_le(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

uint32_t read_u32_le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t dib_palette_bytes(const BITMAPINFOHEADER& bi) {
  if (bi.biBitCount <= 8) {
    const uint32_t clr =
        bi.biClrUsed != 0 ? bi.biClrUsed : (1u << bi.biBitCount);
    return clr * 4u;
  }
  if (bi.biCompression == BI_BITFIELDS &&
      (bi.biBitCount == 16 || bi.biBitCount == 32)) {
    return 12u;  // three DWORD masks
  }
  return 0;
}

bool dib_expected_size(const uint8_t* dib, size_t n, size_t* total_out) {
  if (!dib || !total_out || n < sizeof(BITMAPINFOHEADER)) {
    return false;
  }
  BITMAPINFOHEADER bi{};
  std::memcpy(&bi, dib, sizeof(bi));
  if (bi.biSize < sizeof(BITMAPINFOHEADER) || bi.biSize > n) {
    return false;
  }
  if (bi.biWidth == 0 || bi.biPlanes != 1) {
    return false;
  }
  const int abs_h = bi.biHeight >= 0 ? bi.biHeight : -bi.biHeight;
  if (abs_h <= 0 || abs_h > 16384 || bi.biWidth > 16384) {
    return false;
  }
  const uint32_t pal = dib_palette_bytes(bi);
  const size_t header_and_pal = static_cast<size_t>(bi.biSize) + pal;
  if (header_and_pal > n || header_and_pal > kMaxClipboardBitmapBytes) {
    return false;
  }
  uint32_t img = bi.biSizeImage;
  if (img == 0) {
    if (bi.biCompression != BI_RGB && bi.biCompression != BI_BITFIELDS) {
      return false;
    }
    const uint32_t bpp = bi.biBitCount;
    if (bpp == 0 || bpp > 32) {
      return false;
    }
    const uint32_t row =
        ((static_cast<uint32_t>(bi.biWidth) * bpp + 31u) / 32u) * 4u;
    img = row * static_cast<uint32_t>(abs_h);
  }
  const size_t total = header_and_pal + img;
  if (total == 0 || total > kMaxClipboardBitmapBytes || total > n) {
    return false;
  }
  *total_out = total;
  return true;
}

bool convert_hbitmap_to_dib(HBITMAP hbmp, std::vector<uint8_t>* dib_out) {
  if (!hbmp || !dib_out) {
    return false;
  }
  BITMAP bm{};
  if (GetObjectW(hbmp, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
    return false;
  }
  BITMAPINFOHEADER bi{};
  bi.biSize = sizeof(BITMAPINFOHEADER);
  bi.biWidth = bm.bmWidth;
  bi.biHeight = bm.bmHeight;  // bottom-up
  bi.biPlanes = 1;
  bi.biBitCount = 32;
  bi.biCompression = BI_RGB;

  const uint32_t row = ((static_cast<uint32_t>(bm.bmWidth) * 32u + 31u) / 32u) * 4u;
  const uint32_t img = row * static_cast<uint32_t>(bm.bmHeight);
  const size_t total = sizeof(BITMAPINFOHEADER) + img;
  if (total > kMaxClipboardBitmapBytes) {
    return false;
  }

  dib_out->assign(total, 0);
  std::memcpy(dib_out->data(), &bi, sizeof(bi));

  HDC hdc = GetDC(nullptr);
  if (!hdc) {
    dib_out->clear();
    return false;
  }
  BITMAPINFO bmi{};
  bmi.bmiHeader = bi;
  const int got =
      GetDIBits(hdc, hbmp, 0, static_cast<UINT>(bm.bmHeight), dib_out->data() + sizeof(bi),
                &bmi, DIB_RGB_COLORS);
  ReleaseDC(nullptr, hdc);
  if (got != bm.bmHeight) {
    dib_out->clear();
    return false;
  }
  // GetDIBits may rewrite header fields (biSizeImage etc.).
  std::memcpy(dib_out->data(), &bmi.bmiHeader, sizeof(BITMAPINFOHEADER));
  return true;
}

}  // namespace

void ClipboardEchoGuard::mark_local_write(uint32_t text_digest) {
  ignore_seq = GetClipboardSequenceNumber();
  last_text_digest = text_digest;
}

void ClipboardEchoGuard::mark_local_bitmap_write(uint32_t bitmap_digest) {
  ignore_seq = GetClipboardSequenceNumber();
  last_bitmap_digest = bitmap_digest;
}

bool ClipboardEchoGuard::should_skip_seq(DWORD seq) const {
  return seq != 0 && seq == ignore_seq;
}

uint32_t clipboard_text_digest(const uint8_t* utf16, size_t byte_len) {
  return clipboard_bytes_digest(utf16, byte_len);
}

uint32_t clipboard_bytes_digest(const uint8_t* data, size_t byte_len) {
  uint32_t h = 2166136261u;
  if (!data || byte_len == 0) {
    return h;
  }
  for (size_t i = 0; i < byte_len; ++i) {
    h ^= data[i];
    h *= 16777619u;
  }
  h ^= static_cast<uint32_t>(byte_len);
  return h;
}

bool clipboard_has_hdrop() {
  return IsClipboardFormatAvailable(CF_HDROP) != 0;
}

bool clipboard_has_dib() {
  return IsClipboardFormatAvailable(CF_DIB) != 0 ||
         IsClipboardFormatAvailable(CF_BITMAP) != 0;
}

bool clipboard_has_text() {
  return IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
}

bool clipboard_read_text_utf16(std::vector<uint8_t>* utf16_out) {
  if (!utf16_out) {
    return false;
  }
  utf16_out->clear();
  if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
    return false;
  }
  if (!OpenClipboard(nullptr)) {
    return false;
  }
  bool ok = false;
  HANDLE h = GetClipboardData(CF_UNICODETEXT);
  if (h) {
    const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h));
    if (w) {
      size_t nchars = 0;
      while (w[nchars] != L'\0') {
        ++nchars;
        if (nchars > (kMaxClipboardTextBytes / sizeof(wchar_t))) {
          nchars = 0;
          break;
        }
      }
      if (nchars > 0) {
        const size_t nbytes = nchars * sizeof(wchar_t);
        if (nbytes <= kMaxClipboardTextBytes) {
          utf16_out->assign(reinterpret_cast<const uint8_t*>(w),
                            reinterpret_cast<const uint8_t*>(w) + nbytes);
          ok = true;
        }
      }
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return ok;
}

bool clipboard_write_text_utf16(const uint8_t* utf16, size_t byte_len, ClipboardEchoGuard* guard) {
  if (!utf16 || byte_len == 0 || (byte_len % 2) != 0 || byte_len > kMaxClipboardTextBytes) {
    return false;
  }
  if (!OpenClipboard(nullptr)) {
    return false;
  }
  EmptyClipboard();
  const SIZE_T alloc = byte_len + sizeof(wchar_t);
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, alloc);
  if (!h) {
    CloseClipboard();
    return false;
  }
  void* p = GlobalLock(h);
  if (!p) {
    GlobalFree(h);
    CloseClipboard();
    return false;
  }
  std::memcpy(p, utf16, byte_len);
  *reinterpret_cast<wchar_t*>(static_cast<uint8_t*>(p) + byte_len) = L'\0';
  GlobalUnlock(h);
  if (!SetClipboardData(CF_UNICODETEXT, h)) {
    GlobalFree(h);
    CloseClipboard();
    return false;
  }
  CloseClipboard();
  if (guard) {
    guard->mark_local_write(clipboard_text_digest(utf16, byte_len));
  }
  return true;
}

bool clipboard_read_dib(std::vector<uint8_t>* dib_out) {
  if (!dib_out) {
    return false;
  }
  dib_out->clear();
  if (!OpenClipboard(nullptr)) {
    return false;
  }
  bool ok = false;
  HANDLE h = GetClipboardData(CF_DIB);
  if (h) {
    const SIZE_T sz = GlobalSize(h);
    const uint8_t* p = static_cast<const uint8_t*>(GlobalLock(h));
    if (p && sz >= sizeof(BITMAPINFOHEADER) && sz <= kMaxClipboardBitmapBytes) {
      size_t expect = 0;
      if (dib_expected_size(p, static_cast<size_t>(sz), &expect)) {
        dib_out->assign(p, p + expect);
        ok = true;
      }
    }
    if (p) {
      GlobalUnlock(h);
    }
  }
  if (!ok) {
    HBITMAP hbmp = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
    if (hbmp) {
      ok = convert_hbitmap_to_dib(hbmp, dib_out);
    }
  }
  CloseClipboard();
  return ok;
}

bool clipboard_write_dib(const uint8_t* dib, size_t byte_len, ClipboardEchoGuard* guard) {
  size_t expect = 0;
  if (!dib || !dib_expected_size(dib, byte_len, &expect) || expect != byte_len) {
    return false;
  }
  if (!OpenClipboard(nullptr)) {
    return false;
  }
  EmptyClipboard();
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, byte_len);
  if (!h) {
    CloseClipboard();
    return false;
  }
  void* p = GlobalLock(h);
  if (!p) {
    GlobalFree(h);
    CloseClipboard();
    return false;
  }
  std::memcpy(p, dib, byte_len);
  GlobalUnlock(h);
  if (!SetClipboardData(CF_DIB, h)) {
    GlobalFree(h);
    CloseClipboard();
    return false;
  }
  CloseClipboard();
  if (guard) {
    guard->mark_local_bitmap_write(clipboard_bytes_digest(dib, byte_len));
  }
  return true;
}

bool build_clip_text_payload(const std::vector<uint8_t>& utf16, std::vector<uint8_t>* out) {
  if (!out || utf16.empty() || (utf16.size() % 2) != 0 || utf16.size() > kMaxClipboardTextBytes) {
    return false;
  }
  out->resize(1 + 4 + utf16.size());
  (*out)[0] = kClipText;
  write_u32_le(out->data() + 1, static_cast<uint32_t>(utf16.size()));
  std::memcpy(out->data() + 5, utf16.data(), utf16.size());
  return true;
}

bool parse_clip_text_payload(const uint8_t* p, size_t n, std::vector<uint8_t>* utf16_out) {
  if (!p || !utf16_out || n < 5 || p[0] != kClipText) {
    return false;
  }
  const uint32_t len = read_u32_le(p + 1);
  if (len == 0 || len > kMaxClipboardTextBytes || (len % 2) != 0 || n < 5u + len) {
    return false;
  }
  utf16_out->assign(p + 5, p + 5 + len);
  return true;
}

bool build_clip_bitmap_payload(const std::vector<uint8_t>& dib, std::vector<uint8_t>* out) {
  if (!out || dib.empty() || dib.size() > kMaxClipboardBitmapBytes) {
    return false;
  }
  size_t expect = 0;
  if (!dib_expected_size(dib.data(), dib.size(), &expect) || expect != dib.size()) {
    return false;
  }

  mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(dib.size()));
  std::vector<uint8_t> zbuf(static_cast<size_t>(bound));
  mz_ulong zlen = bound;
  const int zrc =
      mz_compress2(zbuf.data(), &zlen, dib.data(), static_cast<mz_ulong>(dib.size()), MZ_BEST_SPEED);

  const bool use_zlib = (zrc == MZ_OK && zlen > 0 && zlen < dib.size());
  const size_t body_len = use_zlib ? static_cast<size_t>(zlen) : dib.size();
  const size_t total = 1 + 1 + 4 + body_len;
  if (total > kMaxPayloadLen) {
    return false;
  }
  out->resize(total);
  (*out)[0] = kClipBitmap;
  (*out)[1] = use_zlib ? kClipBitmapZlib : kClipBitmapRaw;
  write_u32_le(out->data() + 2, static_cast<uint32_t>(dib.size()));
  if (use_zlib) {
    std::memcpy(out->data() + 6, zbuf.data(), body_len);
  } else {
    std::memcpy(out->data() + 6, dib.data(), body_len);
  }
  return true;
}

bool parse_clip_bitmap_payload(const uint8_t* p, size_t n, std::vector<uint8_t>* dib_out) {
  if (!p || !dib_out || n < 6 || p[0] != kClipBitmap) {
    return false;
  }
  const uint8_t codec = p[1];
  const uint32_t raw_len = read_u32_le(p + 2);
  if (raw_len == 0 || raw_len > kMaxClipboardBitmapBytes || n < 6) {
    return false;
  }
  const size_t body_len = n - 6;
  if (codec == kClipBitmapRaw) {
    if (body_len != raw_len) {
      return false;
    }
    size_t expect = 0;
    if (!dib_expected_size(p + 6, body_len, &expect) || expect != body_len) {
      return false;
    }
    dib_out->assign(p + 6, p + 6 + body_len);
    return true;
  }
  if (codec == kClipBitmapZlib) {
    if (body_len == 0 || body_len > kMaxPayloadLen) {
      return false;
    }
    dib_out->resize(raw_len);
    mz_ulong dest_len = raw_len;
    const int zrc =
        mz_uncompress(dib_out->data(), &dest_len, p + 6, static_cast<mz_ulong>(body_len));
    if (zrc != MZ_OK || dest_len != raw_len) {
      dib_out->clear();
      return false;
    }
    size_t expect = 0;
    if (!dib_expected_size(dib_out->data(), dib_out->size(), &expect) ||
        expect != dib_out->size()) {
      dib_out->clear();
      return false;
    }
    return true;
  }
  return false;
}

}  // namespace road_desk::replace
