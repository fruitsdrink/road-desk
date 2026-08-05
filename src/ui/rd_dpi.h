#pragma once

// DPI helpers — docs/ui-design-system.md §6 (System DPI Aware).

#include "ui/rd_tokens.h"

namespace road_desk::ui {

inline int rd_dpi_from_dc(HDC hdc) {
  const int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
  return dpi > 0 ? dpi : 96;
}

inline int rd_dpi_screen() {
  HDC screen = GetDC(nullptr);
  const int dpi = rd_dpi_from_dc(screen);
  if (screen) {
    ReleaseDC(nullptr, screen);
  }
  return dpi;
}

inline int rd_dpi_hwnd(HWND hwnd) {
  if (!hwnd) {
    return rd_dpi_screen();
  }
  HDC hdc = GetDC(hwnd);
  const int dpi = rd_dpi_from_dc(hdc);
  if (hdc) {
    ReleaseDC(hwnd, hdc);
  }
  return dpi;
}

// Logical DIP (96 DPI px) → physical pixels.
inline int rd_dip(int v, int dpi) {
  return MulDiv(v, dpi > 0 ? dpi : 96, 96);
}

inline int rd_dip(HWND hwnd, int v) {
  return rd_dip(v, rd_dpi_hwnd(hwnd));
}

// Create UI font (Segoe → YaHei UI → Tahoma). Caller DeleteObject.
// weight: FW_NORMAL / FW_SEMIBOLD (600).
inline HFONT rd_create_font(int dpi, int pt, int weight = FW_NORMAL) {
  LOGFONTW lf{};
  lf.lfHeight = -MulDiv(pt, dpi > 0 ? dpi : 96, 72);
  lf.lfWeight = weight;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfQuality = CLEARTYPE_QUALITY;
  lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
  const wchar_t* faces[] = {L"Segoe UI", L"Microsoft YaHei UI", L"Tahoma"};
  for (const wchar_t* face : faces) {
    wcscpy_s(lf.lfFaceName, face);
    if (HFONT font = CreateFontIndirectW(&lf)) {
      return font;
    }
  }
  return nullptr;
}

inline HFONT rd_create_font(HWND hwnd, int pt, int weight = FW_NORMAL) {
  return rd_create_font(rd_dpi_hwnd(hwnd), pt, weight);
}

}  // namespace road_desk::ui
