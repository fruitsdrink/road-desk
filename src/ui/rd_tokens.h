#pragma once

// Road Desk Win32 UI tokens — docs/ui-design-system.md §3
// Viewer and Host Agent must include this instead of inventing RGB/padding.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace road_desk::ui {

// --- rd.color.brand.* ---
inline constexpr COLORREF kColorBrandBg = RGB(22, 30, 46);
inline constexpr COLORREF kColorBrandFg = RGB(255, 255, 255);
inline constexpr COLORREF kColorBrandFgMuted = RGB(160, 176, 200);

// --- rd.color.surface.* ---
inline constexpr COLORREF kColorSurfaceApp = RGB(246, 248, 251);
inline constexpr COLORREF kColorSurfacePanel = RGB(255, 255, 255);
inline constexpr COLORREF kColorSurfaceChrome = RGB(236, 240, 245);
inline constexpr COLORREF kColorSurfaceSession = RGB(0, 0, 0);

// --- rd.color.text.* ---
inline constexpr COLORREF kColorTextPrimary = RGB(28, 34, 46);
inline constexpr COLORREF kColorTextSecondary = RGB(88, 98, 114);
inline constexpr COLORREF kColorTextMuted = RGB(140, 150, 164);
inline constexpr COLORREF kColorTextOnBrand = RGB(255, 255, 255);
inline constexpr COLORREF kColorTextInverseMuted = RGB(160, 176, 200);
inline constexpr COLORREF kColorTextSessionHint = RGB(220, 220, 220);

// --- rd.color.border.* / accent / status ---
inline constexpr COLORREF kColorBorderSubtle = RGB(220, 226, 234);
inline constexpr COLORREF kColorBorderStrong = RGB(180, 190, 204);
inline constexpr COLORREF kColorAccent = RGB(47, 107, 168);
inline constexpr COLORREF kColorAccentHover = RGB(38, 90, 145);
inline constexpr COLORREF kColorAccentPressed = RGB(30, 74, 122);
inline constexpr COLORREF kColorSuccess = RGB(46, 125, 90);
inline constexpr COLORREF kColorWarning = RGB(180, 120, 40);
inline constexpr COLORREF kColorDanger = RGB(176, 60, 60);
inline constexpr COLORREF kColorFocusRing = RGB(47, 107, 168);

// --- rd.color.row / tab / splitter ---
inline constexpr COLORREF kColorRowHover = RGB(232, 238, 246);
inline constexpr COLORREF kColorRowSelected = RGB(210, 226, 244);
inline constexpr COLORREF kColorTabActive = RGB(255, 255, 255);
inline constexpr COLORREF kColorTabInactive = RGB(236, 240, 245);
inline constexpr COLORREF kColorSplitter = RGB(200, 208, 218);

// --- rd.font.* point sizes (96 DPI logical) ---
inline constexpr int kFontBrandPt = 16;
inline constexpr int kFontLoginTitlePt = 18;
inline constexpr int kFontTitlePt = 12;
inline constexpr int kFontBodyPt = 10;
inline constexpr int kFontLabelPt = 9;
inline constexpr int kFontCaptionPt = 9;

// --- rd.space.* (DIP @ 96) ---
inline constexpr int kSpace1 = 4;
inline constexpr int kSpace2 = 8;
inline constexpr int kSpace3 = 12;
inline constexpr int kSpace4 = 16;
inline constexpr int kSpace5 = 20;
inline constexpr int kSpace6 = 24;
inline constexpr int kSpace7 = 28;
inline constexpr int kSpace8 = 32;

// Common control sizes (DIP) — docs §3.5
inline constexpr int kEditHDip = 28;
inline constexpr int kButtonHDip = 30;
inline constexpr int kToolbarHDip = 32;
inline constexpr int kStatusHDip = 24;
inline constexpr int kTreeDefaultWDip = 240;
inline constexpr int kDetailDefaultWDip = 400;
inline constexpr int kSplitterWDip = 4;
inline constexpr int kHeaderHDip = 72;
inline constexpr int kRadiusMdDip = 4;

}  // namespace road_desk::ui
