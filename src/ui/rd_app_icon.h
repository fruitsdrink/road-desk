#pragma once

// Load / apply embedded Road Desk app icon (IDI_RD_APP).

#include "ui/rd_app_res.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace road_desk::ui {

inline HICON rd_load_app_icon(HINSTANCE inst, int cx, int cy) {
  if (!inst) {
    return nullptr;
  }
  return reinterpret_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_RD_APP), IMAGE_ICON, cx, cy,
                                            LR_SHARED));
}

inline void rd_apply_wndclass_icons(WNDCLASSEXW* wc, HINSTANCE inst) {
  if (!wc || !inst) {
    return;
  }
  wc->hIcon = rd_load_app_icon(inst, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
  wc->hIconSm =
      rd_load_app_icon(inst, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
}

inline void rd_apply_wndclass_icons(WNDCLASSW* wc, HINSTANCE inst) {
  if (!wc || !inst) {
    return;
  }
  wc->hIcon = rd_load_app_icon(inst, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
}

inline void rd_set_window_icons(HWND hwnd, HINSTANCE inst) {
  if (!hwnd || !inst) {
    return;
  }
  if (HICON big =
          rd_load_app_icon(inst, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON))) {
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
  }
  if (HICON sm =
          rd_load_app_icon(inst, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON))) {
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(sm));
  }
}

}  // namespace road_desk::ui
