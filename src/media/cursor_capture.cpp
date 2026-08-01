#include "cursor_capture.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

namespace road_desk::replace {
namespace {

uint32_t fnv1a(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

struct Dib {
  HDC dc = nullptr;
  HBITMAP bmp = nullptr;
  HGDIOBJ old = nullptr;
  uint8_t* bits = nullptr;
  int w = 0;
  int h = 0;
};

void dib_destroy(Dib* d) {
  if (!d) {
    return;
  }
  if (d->dc && d->old) {
    SelectObject(d->dc, d->old);
  }
  if (d->bmp) {
    DeleteObject(d->bmp);
  }
  if (d->dc) {
    DeleteDC(d->dc);
  }
  d->dc = nullptr;
  d->bmp = nullptr;
  d->old = nullptr;
  d->bits = nullptr;
}

bool dib_create(HDC screen, int w, int h, Dib* out) {
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  out->w = w;
  out->h = h;
  out->dc = CreateCompatibleDC(screen);
  if (!out->dc) {
    return false;
  }
  void* bits = nullptr;
  out->bmp = CreateDIBSection(out->dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!out->bmp || !bits) {
    dib_destroy(out);
    return false;
  }
  out->bits = static_cast<uint8_t*>(bits);
  out->old = SelectObject(out->dc, out->bmp);
  return true;
}

void dib_fill(Dib* d, COLORREF c) {
  RECT rc{0, 0, d->w, d->h};
  HBRUSH br = CreateSolidBrush(c);
  FillRect(d->dc, &rc, br);
  DeleteObject(br);
}

// Win7 DrawIconEx often leaves alpha=0. Recover opacity by drawing on black and white.
void recover_bgra_alpha(const uint8_t* on_black, const uint8_t* on_white, uint8_t* out, int n) {
  for (int i = 0; i < n; ++i) {
    const uint8_t* b = on_black + static_cast<size_t>(i) * 4u;
    const uint8_t* w = on_white + static_cast<size_t>(i) * 4u;
    uint8_t* o = out + static_cast<size_t>(i) * 4u;
    const bool clear_on_black = (b[0] | b[1] | b[2]) == 0;
    const bool clear_on_white = (w[0] == 255 && w[1] == 255 && w[2] == 255);
    if (clear_on_black && clear_on_white) {
      o[0] = o[1] = o[2] = o[3] = 0;
      continue;
    }
    if (b[0] == w[0] && b[1] == w[1] && b[2] == w[2]) {
      o[0] = b[0];
      o[1] = b[1];
      o[2] = b[2];
      o[3] = 255;
      continue;
    }
    // Partial / XOR-ish: keep black-bg color opaque (avoids viewer XOR inversion).
    o[0] = b[0];
    o[1] = b[1];
    o[2] = b[2];
    o[3] = 255;
  }
}

bool alpha_channel_used(const uint8_t* bgra, size_t bytes) {
  for (size_t i = 3; i < bytes; i += 4) {
    if (bgra[i] != 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool capture_cursor_shape(CursorShape* out) {
  if (!out) {
    return false;
  }
  out->hidden = true;
  out->hot_x = 0;
  out->hot_y = 0;
  out->w = 0;
  out->h = 0;
  out->bgra.clear();
  out->hash = 0;

  CURSORINFO ci{};
  ci.cbSize = sizeof(ci);
  if (!GetCursorInfo(&ci)) {
    return true;
  }
  if (!(ci.flags & CURSOR_SHOWING) || !ci.hCursor) {
    out->hash = 1;  // distinct from empty shape
    return true;
  }

  ICONINFO ii{};
  if (!GetIconInfo(ci.hCursor, &ii)) {
    return true;
  }

  BITMAP bm{};
  HBITMAP shape = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
  if (!shape || !GetObject(shape, sizeof(bm), &bm)) {
    if (ii.hbmColor) {
      DeleteObject(ii.hbmColor);
    }
    if (ii.hbmMask) {
      DeleteObject(ii.hbmMask);
    }
    return true;
  }

  int w = bm.bmWidth;
  int h = bm.bmHeight;
  // Mono cursor: mask is double-height (AND + XOR).
  if (!ii.hbmColor && h > 0) {
    h /= 2;
  }
  if (w <= 0 || h <= 0 || w > 256 || h > 256) {
    if (ii.hbmColor) {
      DeleteObject(ii.hbmColor);
    }
    if (ii.hbmMask) {
      DeleteObject(ii.hbmMask);
    }
    return true;
  }

  HDC screen = GetDC(nullptr);
  Dib black{};
  Dib white{};
  bool ok = screen && dib_create(screen, w, h, &black) && dib_create(screen, w, h, &white);
  if (!ok) {
    dib_destroy(&black);
    dib_destroy(&white);
    if (screen) {
      ReleaseDC(nullptr, screen);
    }
    if (ii.hbmColor) {
      DeleteObject(ii.hbmColor);
    }
    if (ii.hbmMask) {
      DeleteObject(ii.hbmMask);
    }
    return false;
  }

  dib_fill(&black, RGB(0, 0, 0));
  DrawIconEx(black.dc, 0, 0, ci.hCursor, w, h, 0, nullptr, DI_NORMAL);
  dib_fill(&white, RGB(255, 255, 255));
  DrawIconEx(white.dc, 0, 0, ci.hCursor, w, h, 0, nullptr, DI_NORMAL);

  out->hidden = false;
  out->hot_x = static_cast<uint16_t>(ii.xHotspot);
  out->hot_y = static_cast<uint16_t>(ii.yHotspot);
  out->w = static_cast<uint16_t>(w);
  out->h = static_cast<uint16_t>(h);
  out->bgra.resize(static_cast<size_t>(w) * h * 4u);

  // Prefer DrawIconEx alpha when present (Vista+ color cursors); else recover.
  std::memcpy(out->bgra.data(), black.bits, out->bgra.size());
  if (!alpha_channel_used(out->bgra.data(), out->bgra.size())) {
    recover_bgra_alpha(black.bits, white.bits, out->bgra.data(), w * h);
  } else {
    // Transparent pixels must be zero RGB so CreateIconIndirect never XOR-bleeds.
    for (size_t i = 0; i < out->bgra.size(); i += 4) {
      if (out->bgra[i + 3] < 16) {
        out->bgra[i] = out->bgra[i + 1] = out->bgra[i + 2] = out->bgra[i + 3] = 0;
      }
    }
  }

  out->hash = fnv1a(out->bgra.data(), out->bgra.size()) ^
              (static_cast<uint32_t>(out->hot_x) << 16) ^ out->hot_y ^
              (static_cast<uint32_t>(w) << 8) ^ h;

  dib_destroy(&black);
  dib_destroy(&white);
  ReleaseDC(nullptr, screen);
  if (ii.hbmColor) {
    DeleteObject(ii.hbmColor);
  }
  if (ii.hbmMask) {
    DeleteObject(ii.hbmMask);
  }
  return true;
}

}  // namespace road_desk::replace
