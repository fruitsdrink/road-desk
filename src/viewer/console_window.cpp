#include "console_window.h"

#include "address_book.h"
#include "audit_client.h"
#include "product_version.h"
#include "session_host.h"
#include "ui/rd_app_icon.h"
#include "ui/rd_dpi.h"
#include "ui/rd_tokens.h"

#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace road_desk::viewer {
namespace {

namespace gdip = Gdiplus;
namespace rd = road_desk::ui;

constexpr wchar_t kConsoleClass[] = L"RoadDeskConsoleWindow";
constexpr wchar_t kTabStripClass[] = L"RoadDeskTabStrip";
constexpr wchar_t kDragGhostClass[] = L"RoadDeskDragGhost";
constexpr wchar_t kSplitterChipClass[] = L"RoadDeskSplitterChip";
constexpr int kToolbarHDip = rd::kToolbarHDip;
constexpr int kToolbarIconDip = 16;
constexpr int kToolbarPadXDip = rd::kSpace2;
constexpr int kStatusHDip = rd::kStatusHDip;
constexpr int kTabHDefault = 30;
constexpr int kCloseBtnWDip = 18;
constexpr int kSplitterWDip = rd::kSplitterWDip;
constexpr int kTabNavWDip = 24;  // overflow ◀▶ (design Frame E)
constexpr int kFloatBadgeWDip = 14;
constexpr int kSplitterChipWDip = 72;  // design Frame D width chip
constexpr int kSplitterChipHDip = 24;
constexpr int kSplitterChipGapDip = 12;
constexpr int kMaxSessions = 8;
constexpr int kTabCatalog = -1;
constexpr int kTabHitClose = -3;  // session close button (drag_tab_index holds session idx)
constexpr int kTabHitNavLeft = -4;
constexpr int kTabHitNavRight = -5;
constexpr int kGhostW = 260;
constexpr int kGhostH = 160;
constexpr int kGhostTitleH = 28;

// Aliases → shared tokens (docs/ui-design-system.md).
constexpr COLORREF kChrome = rd::kColorSurfaceChrome;
constexpr COLORREF kPanel = rd::kColorSurfacePanel;
constexpr COLORREF kBorderSubtle = rd::kColorBorderSubtle;
constexpr COLORREF kBorderStrong = rd::kColorBorderStrong;
constexpr COLORREF kTextPrimary = rd::kColorTextPrimary;
constexpr COLORREF kTextSecondary = rd::kColorTextSecondary;
constexpr COLORREF kTextMuted = rd::kColorTextMuted;
constexpr COLORREF kIconInk = rd::kColorTextSecondary;
constexpr COLORREF kRowHover = rd::kColorRowHover;
constexpr COLORREF kRowSelected = rd::kColorRowSelected;
constexpr COLORREF kAccent = rd::kColorAccent;
constexpr COLORREF kAccentHover = rd::kColorAccentHover;
constexpr COLORREF kAccentPressed = rd::kColorAccentPressed;
constexpr COLORREF kSplitter = rd::kColorSplitter;
constexpr COLORREF kTabActive = rd::kColorTabActive;
constexpr COLORREF kTabInactive = rd::kColorTabInactive;

enum : int {
  IDC_TREE = 1001,
  IDC_LIST = 1002,
  IDC_TABSTRIP = 1003,
  IDC_STATUS = 1004,
  IDC_TOOLBAR = 1005,
  IDC_SESSION_HOST = 1006,
};

// Menu + toolbar command ids (shared where actions overlap).
constexpr int kCmdRefresh = 10;
constexpr int kCmdSession = 11;       // toolbar: start (list) / close (active tab)
constexpr int kCmdCloseAll = 12;
constexpr int kCmdScreenshot = 13;
constexpr int kCmdFullscreen = 14;
constexpr int kCmdViewOnly = 15;
constexpr int kCmdExit = 16;
constexpr int kCmdAbout = 17;
constexpr int kCmdStartSession = 18;  // menu: start / focus selected device
constexpr int kCmdCloseSession = 19;  // menu: close active tab
constexpr int kCmdNextTab = 20;
constexpr int kCmdPrevTab = 21;
constexpr int kCmdCyclePane = 22;

// Toolbar imagelist indices (build_toolbar_images order).
constexpr int kTbImgRefresh = 0;
constexpr int kTbImgClose = 1;
constexpr int kTbImgCloseAll = 2;
constexpr int kTbImgScreenshot = 3;
constexpr int kTbImgFullscreen = 4;
constexpr int kTbImgViewOnly = 5;
constexpr int kTbImgStart = 6;

// Tab context menu (design Frame C).
constexpr int kCmdTabCtxClose = 2001;
constexpr int kCmdTabCtxCloseOthers = 2002;
constexpr int kCmdTabCtxCloseAll = 2003;
constexpr int kCmdTabCtxTear = 2004;
constexpr int kCmdTabCtxDock = 2005;
constexpr int kCmdTabCtxFullscreen = 2006;
constexpr int kCmdTabCtxViewOnly = 2007;

struct SessionTab {
  int device_id = -1;
  std::wstring title;
  std::unique_ptr<SessionHost> host;
  bool floating = false;
};

struct ConsoleState {
  HINSTANCE instance = nullptr;
  HWND hwnd = nullptr;
  HWND toolbar = nullptr;
  HIMAGELIST toolbar_il = nullptr;
  HIMAGELIST toolbar_il_on_accent = nullptr;  // white glyphs for checked (accent) face
  HIMAGELIST toolbar_il_muted = nullptr;      // text.muted for disabled
  HWND tree = nullptr;
  HWND list = nullptr;
  HWND tab_strip = nullptr;
  HWND status = nullptr;
  HWND session_area = nullptr;
  HWND splitter_chip = nullptr;  // Frame D: DIP width while dragging
  HFONT ui_font = nullptr;       // rd.font.body 10pt
  HFONT caption_font = nullptr;  // rd.font.caption 9pt
  HBRUSH menu_chrome_br = nullptr;
  HACCEL accel = nullptr;
  HBITMAP menu_icons[6] = {};  // MenuBarIcon order
  ConnectDefaults connect;
  int dpi = 96;
  int toolbar_h = kToolbarHDip;
  int status_h = kStatusHDip;
  int splitter_w = kSplitterWDip;
  int close_btn_w = kCloseBtnWDip;
  int tab_nav_w = kTabNavWDip;
  int float_badge_w = kFloatBadgeWDip;
  int tree_width = rd::kTreeDefaultWDip;
  int tab_h = kTabHDefault;
  int selected_group_id = 0;
  int active_tab = kTabCatalog;  // kTabCatalog or session index
  int toolbar_hot_cmd = -1;      // for hover transition invalidate
  std::vector<SessionTab> sessions;
  bool dragging_splitter = false;
  bool splitter_hot = false;
  int body_top = 0;  // below toolbar; for splitter paint/hit
  int splitter_chip_dip = 0;  // logical width shown on chip
  int tab_scroll_x = 0;
  bool dragging_tab = false;
  bool drag_tear_armed = false;  // moved far enough; detach only on LBUTTONUP
  int drag_tab_index = -1;
  int close_hit_index = -1;  // session index when close button pressed
  POINT drag_start{};
  HWND drag_ghost = nullptr;
  std::wstring drag_ghost_title;
};

ConsoleState* g_console = nullptr;

int system_dpi() {
  return rd::rd_dpi_screen();
}

int dip(int v, int dpi) {
  return rd::rd_dip(v, dpi);
}

gdip::Color gdip_rgb(COLORREF c, BYTE a = 255) {
  return gdip::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

void fill_round_rect(HDC hdc, const RECT& rc, COLORREF fill, COLORREF border, int diameter) {
  if (!hdc || rc.right - rc.left < 2 || rc.bottom - rc.top < 2) {
    return;
  }
  HPEN pen = CreatePen(PS_SOLID, 1, border);
  HBRUSH br = CreateSolidBrush(fill);
  HGDIOBJ old_pen = SelectObject(hdc, pen);
  HGDIOBJ old_br = SelectObject(hdc, br);
  RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, diameter, diameter);
  SelectObject(hdc, old_pen);
  SelectObject(hdc, old_br);
  DeleteObject(pen);
  DeleteObject(br);
}

// Frame D: tree width chip (DIP), shown only while dragging the splitter.
void hide_splitter_chip(ConsoleState* st) {
  if (st && st->splitter_chip) {
    ShowWindow(st->splitter_chip, SW_HIDE);
  }
}

void update_splitter_chip(ConsoleState* st) {
  if (!st || !st->hwnd || !st->splitter_chip) {
    return;
  }
  if (!st->dragging_splitter) {
    hide_splitter_chip(st);
    return;
  }
  RECT client{};
  GetClientRect(st->hwnd, &client);
  const int dpi = st->dpi > 0 ? st->dpi : 96;
  const int top = st->body_top > 0 ? st->body_top : st->toolbar_h;
  const int bottom = client.bottom - st->status_h;
  if (bottom <= top) {
    hide_splitter_chip(st);
    return;
  }
  st->splitter_chip_dip = MulDiv(st->tree_width, 96, dpi);
  wchar_t label[32];
  swprintf_s(label, L"%d px", st->splitter_chip_dip);

  HDC hdc = GetDC(st->splitter_chip);
  HFONT font = st->caption_font ? st->caption_font : st->ui_font;
  HGDIOBJ old = font && hdc ? SelectObject(hdc, font) : nullptr;
  SIZE tsz{0, 0};
  if (hdc) {
    GetTextExtentPoint32W(hdc, label, static_cast<int>(wcslen(label)), &tsz);
  }
  if (old) {
    SelectObject(hdc, old);
  }
  if (hdc) {
    ReleaseDC(st->splitter_chip, hdc);
  }

  const int pad_x = dip(rd::kSpace2 + 2, dpi);
  const int chip_h = dip(kSplitterChipHDip, dpi);
  int chip_w = dip(kSplitterChipWDip, dpi);
  if (tsz.cx + pad_x * 2 > chip_w) {
    chip_w = tsz.cx + pad_x * 2;
  }
  const int gap = dip(kSplitterChipGapDip, dpi);
  int x = st->tree_width + st->splitter_w + gap;
  if (x + chip_w > client.right - gap) {
    x = st->tree_width - gap - chip_w;
  }
  if (x < gap) {
    x = gap;
  }
  const int y = top + (bottom - top - chip_h) / 2;
  SetWindowTextW(st->splitter_chip, label);
  SetWindowPos(st->splitter_chip, HWND_TOP, x, y, chip_w, chip_h,
               SWP_SHOWWINDOW | SWP_NOACTIVATE);
  InvalidateRect(st->splitter_chip, nullptr, FALSE);
}

LRESULT CALLBACK SplitterChipProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  ConsoleState* st = g_console;
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      const int dpi = st && st->dpi > 0 ? st->dpi : 96;
      fill_round_rect(hdc, rc, rd::kColorBrandBg, rd::kColorBrandBg, dip(rd::kRadiusMdDip, dpi) * 2);
      wchar_t label[32] = {};
      GetWindowTextW(hwnd, label, 32);
      HFONT font = st && st->caption_font ? st->caption_font : (st ? st->ui_font : nullptr);
      HGDIOBJ old = font ? SelectObject(hdc, font) : nullptr;
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, rd::kColorTextOnBrand);
      DrawTextW(hdc, label, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (old) {
        SelectObject(hdc, old);
      }
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void paint_toolbar_separator(HDC hdc, const RECT& rc, int dpi) {
  const int mid = (rc.left + rc.right) / 2;
  const int pad = dip(8, dpi);
  HPEN pen = CreatePen(PS_SOLID, 1, kBorderSubtle);
  HGDIOBJ old = SelectObject(hdc, pen);
  MoveToEx(hdc, mid, rc.top + pad, nullptr);
  LineTo(hdc, mid, rc.bottom - pad);
  SelectObject(hdc, old);
  DeleteObject(pen);
}

// Design (viewer-console.pen Frame A/B):
// - disabled: no face, muted glyph (TB_SETDISABLEDIMAGELIST)
// - enabled idle: white panel + border.subtle, 32×28, radius 4
// - hover: panel + border.strong
// - pressed: row.selected + accent.pressed border
// - checked (只读): accent fill + white glyph
LRESULT paint_toolbar_button(ConsoleState* st, LPNMTBCUSTOMDRAW cd) {
  if (!st || !cd) {
    return CDRF_DODEFAULT;
  }
  if (cd->nmcd.dwItemSpec == 0) {
    paint_toolbar_separator(cd->nmcd.hdc, cd->nmcd.rc, st->dpi);
    return CDRF_SKIPDEFAULT;
  }

  const UINT state = cd->nmcd.uItemState;
  const bool disabled = (state & CDIS_DISABLED) != 0;
  const bool pressed = (state & CDIS_SELECTED) != 0;
  const bool hot = (state & CDIS_HOT) != 0;
  const bool checked = (state & CDIS_CHECKED) != 0;

  RECT face = cd->nmcd.rc;
  // Design face 32×28 inside 32-tall strip → 2 DIP vertical inset, full button width.
  const int iy = dip(2, st->dpi);
  face.top += iy;
  face.bottom -= iy;

  const int diameter = dip(4, st->dpi) * 2;  // rd.radius.md

  if (!disabled) {
    COLORREF fill = kPanel;
    COLORREF border = kBorderSubtle;
    if (checked) {
      fill = kAccent;
      border = kAccent;
    } else if (pressed) {
      fill = kRowSelected;
      border = kAccentPressed;
      OffsetRect(&face, dip(1, st->dpi), dip(1, st->dpi));
    } else if (hot) {
      fill = kPanel;
      border = kBorderStrong;
    }
    fill_round_rect(cd->nmcd.hdc, face, fill, border, diameter);
  }

  if (checked && !disabled && st->toolbar_il_on_accent) {
    TBBUTTONINFOW bi{};
    bi.cbSize = sizeof(bi);
    bi.dwMask = TBIF_IMAGE;
    if (SendMessageW(st->toolbar, TB_GETBUTTONINFOW, cd->nmcd.dwItemSpec,
                     reinterpret_cast<LPARAM>(&bi)) >= 0 &&
        bi.iImage >= 0) {
      HIMAGELIST il = st->toolbar_il_on_accent;
      int iw = 0;
      int ih = 0;
      ImageList_GetIconSize(il, &iw, &ih);
      const int ix = (cd->nmcd.rc.left + cd->nmcd.rc.right - iw) / 2;
      const int iy_icon = (cd->nmcd.rc.top + cd->nmcd.rc.bottom - ih) / 2;
      ImageList_Draw(il, bi.iImage, cd->nmcd.hdc, ix, iy_icon, ILD_TRANSPARENT);
      return CDRF_SKIPDEFAULT;
    }
  }

  return TBCDRF_NOBACKGROUND | TBCDRF_NOEDGES;
}

std::wstring utf8_to_wide_local(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
  if (n > 1) {
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  }
  return w;
}

const wchar_t* catalog_ready_status() {
  return address_book_source() == AddressBookSource::kGateway ? L"网关目录 — Ready"
                                                             : L"演示模式 — Ready";
}

void destroy_drag_ghost(ConsoleState* st) {
  if (!st || !st->drag_ghost) {
    return;
  }
  DestroyWindow(st->drag_ghost);
  st->drag_ghost = nullptr;
  st->drag_ghost_title.clear();
}

void move_drag_ghost(ConsoleState* st, POINT screen_pt) {
  if (!st || !st->drag_ghost) {
    return;
  }
  // Offset so the cursor sits on the fake title bar, like dragging a window.
  SetWindowPos(st->drag_ghost, HWND_TOPMOST, screen_pt.x - 40, screen_pt.y - 14, 0, 0,
               SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

LRESULT CALLBACK DragGhostProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      // Outer frame
      FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(GetSysColorBrush(COLOR_WINDOW)));
      FrameRect(hdc, &rc, reinterpret_cast<HBRUSH>(GetSysColorBrush(COLOR_WINDOWFRAME)));

      RECT title = rc;
      title.bottom = title.top + kGhostTitleH;
      FillRect(hdc, &title, reinterpret_cast<HBRUSH>(GetSysColorBrush(COLOR_ACTIVECAPTION)));

      ConsoleState* st = g_console;
      HFONT font = st ? st->ui_font : nullptr;
      HGDIOBJ old = font ? SelectObject(hdc, font) : nullptr;
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, GetSysColor(COLOR_CAPTIONTEXT));
      RECT text_rc = title;
      text_rc.left += 8;
      text_rc.right -= 8;
      const wchar_t* title_txt =
          (st && !st->drag_ghost_title.empty()) ? st->drag_ghost_title.c_str() : L"会话";
      DrawTextW(hdc, title_txt, -1, &text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

      RECT body = rc;
      body.top = title.bottom;
      InflateRect(&body, -8, -8);
      SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
      DrawTextW(hdc, L"拖出为独立窗口…", -1, &body,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (old) {
        SelectObject(hdc, old);
      }
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_NCHITTEST:
      return HTTRANSPARENT;  // never steal mouse from tab strip capture
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void ensure_drag_ghost(ConsoleState* st, const std::wstring& title) {
  if (!st) {
    return;
  }
  st->drag_ghost_title = title;
  if (st->drag_ghost) {
    InvalidateRect(st->drag_ghost, nullptr, TRUE);
    return;
  }
  st->drag_ghost = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
      kDragGhostClass, L"", WS_POPUP, 0, 0, kGhostW, kGhostH, st->hwnd, nullptr, st->instance,
      nullptr);
  if (!st->drag_ghost) {
    return;
  }
  SetLayeredWindowAttributes(st->drag_ghost, 0, 210, LWA_ALPHA);
  ShowWindow(st->drag_ghost, SW_SHOWNOACTIVATE);
}

HFONT create_ui_font(int dpi, int pt) {
  return rd::rd_create_font(dpi, pt, FW_NORMAL);
}

void apply_ui_font(ConsoleState* st, HWND child) {
  if (st && st->ui_font && child) {
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(st->ui_font), TRUE);
  }
}

void apply_caption_font(ConsoleState* st, HWND child) {
  if (st && st->caption_font && child) {
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(st->caption_font), TRUE);
  }
}

int measure_tab_height(HFONT font) {
  HDC hdc = GetDC(nullptr);
  HGDIOBJ old = font ? SelectObject(hdc, font) : nullptr;
  TEXTMETRICW tm{};
  GetTextMetricsW(hdc, &tm);
  if (old) {
    SelectObject(hdc, old);
  }
  ReleaseDC(nullptr, hdc);
  const int h = tm.tmHeight + 10;
  return h < kTabHDefault ? kTabHDefault : h;
}

void layout_status_parts(ConsoleState* st, int client_w) {
  if (!st || !st->status) {
    return;
  }
  (void)client_w;
  // Design: single chrome caption strip (no version pane / size grip).
  int parts[1] = {-1};
  SendMessageW(st->status, SB_SETPARTS, 1, reinterpret_cast<LPARAM>(parts));
}

void set_status(ConsoleState* st, const wchar_t* text) {
  if (!st || !st->status) {
    return;
  }
  SendMessageW(st->status, SB_SETTEXTW, 0 | SBT_NOBORDERS,
               reinterpret_cast<LPARAM>(text ? text : L""));
}

void sync_chrome_commands(ConsoleState* st);
int find_session_by_device(ConsoleState* st, int device_id);
int selected_list_device_id(ConsoleState* st);
bool session_toolbar_starts(ConsoleState* st);

void sync_session_floating_flags(ConsoleState* st) {
  if (!st) {
    return;
  }
  for (SessionTab& tab : st->sessions) {
    if (tab.host) {
      tab.floating = tab.host->floating();
    }
  }
}

void layout(ConsoleState* st) {
  if (!st || !st->hwnd) {
    return;
  }
  sync_session_floating_flags(st);
  RECT rc{};
  GetClientRect(st->hwnd, &rc);
  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;
  const int menu_pad = 0;
  int y = menu_pad;
  if (st->toolbar) {
    // Full-width chrome strip (do not TB_AUTOSIZE — that shrinks HWND to button cluster).
    MoveWindow(st->toolbar, 0, y, cw, st->toolbar_h, TRUE);
    const int btn = st->toolbar_h;
    SendMessageW(st->toolbar, TB_SETBUTTONSIZE, 0, MAKELONG(btn, btn));
    sync_chrome_commands(st);
    y += st->toolbar_h;
  }
  const int body_h = ch - y - st->status_h;
  if (body_h < 1) {
    return;
  }
  int tw = st->tree_width;
  if (tw < 120) {
    tw = 120;
  }
  if (tw > cw - 200) {
    tw = cw - 200;
  }
  st->tree_width = tw;
  st->body_top = y;
  MoveWindow(st->tree, 0, y, tw, body_h, TRUE);

  // Splitter strip between tree and work area.
  {
    RECT split_rc{tw, y, tw + st->splitter_w, y + body_h};
    InvalidateRect(st->hwnd, &split_rc, FALSE);
  }

  const int work_x = tw + st->splitter_w;
  const int work_w = cw - work_x;
  const bool show_tabs = !st->sessions.empty();
  int content_y = y;
  if (show_tabs && st->tab_strip) {
    ShowWindow(st->tab_strip, SW_SHOW);
    MoveWindow(st->tab_strip, work_x, y, work_w, st->tab_h, TRUE);
    content_y = y + st->tab_h;
    InvalidateRect(st->tab_strip, nullptr, FALSE);
  } else if (st->tab_strip) {
    ShowWindow(st->tab_strip, SW_HIDE);
  }
  const int content_h = y + body_h - content_y;

  const bool show_list = (st->active_tab == kTabCatalog);
  if (st->list) {
    ShowWindow(st->list, show_list ? SW_SHOW : SW_HIDE);
    if (show_list) {
      MoveWindow(st->list, work_x, content_y, work_w, content_h, TRUE);
    }
  }
  if (st->session_area) {
    ShowWindow(st->session_area, show_list ? SW_HIDE : SW_SHOW);
    if (!show_list) {
      MoveWindow(st->session_area, work_x, content_y, work_w, content_h, TRUE);
      if (st->active_tab >= 0 && st->active_tab < static_cast<int>(st->sessions.size())) {
        SessionTab& tab = st->sessions[static_cast<size_t>(st->active_tab)];
        if (tab.host && tab.host->hwnd() && !tab.floating) {
          MoveWindow(tab.host->hwnd(), 0, 0, work_w, content_h, TRUE);
          ShowWindow(tab.host->hwnd(), SW_SHOW);
        }
      }
      for (int i = 0; i < static_cast<int>(st->sessions.size()); ++i) {
        SessionTab& tab = st->sessions[static_cast<size_t>(i)];
        if (!tab.host || !tab.host->hwnd() || tab.floating) {
          continue;
        }
        if (i != st->active_tab) {
          ShowWindow(tab.host->hwnd(), SW_HIDE);
        }
      }
    }
  }
  if (st->status) {
    MoveWindow(st->status, 0, ch - st->status_h, cw, st->status_h, TRUE);
    layout_status_parts(st, cw);
  }
}

void ensure_list_selection(ConsoleState* st) {
  if (!st || !st->list) {
    return;
  }
  if (ListView_GetItemCount(st->list) <= 0) {
    return;
  }
  if (ListView_GetNextItem(st->list, -1, LVNI_SELECTED) >= 0) {
    return;
  }
  ListView_SetItemState(st->list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                        LVIS_SELECTED | LVIS_FOCUSED);
  sync_chrome_commands(st);
}

void fill_list(ConsoleState* st) {
  if (!st || !st->list) {
    return;
  }
  ListView_DeleteAllItems(st->list);
  auto devices = address_book_devices_under(st->selected_group_id);
  int row = 0;
  for (const BookNode* d : devices) {
    std::wstring ip = !d->host.empty() ? utf8_to_wide_local(d->host)
                                       : connect_host_display(st->connect);
    std::wstring ver;
    if (!d->version.empty()) {
      ver = utf8_to_wide_local(d->version);
    } else {
      wchar_t buf[32];
      _snwprintf_s(buf, _TRUNCATE, L"%hs", ROAD_DESK_VERSION_STRING);
      ver = buf;
    }
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = row;
    item.pszText = const_cast<wchar_t*>(d->name.c_str());
    item.lParam = d->id;
    ListView_InsertItem(st->list, &item);
    ListView_SetItemText(st->list, row, 1, const_cast<LPWSTR>(ip.c_str()));
    ListView_SetItemText(st->list, row, 2, const_cast<LPWSTR>(ver.c_str()));
    ListView_SetItemText(st->list, row, 3, const_cast<LPWSTR>(d->role.c_str()));
    ListView_SetItemText(st->list, row, 4, const_cast<LPWSTR>(d->remark.c_str()));
    ++row;
  }
  if (row > 0) {
    ListView_SetItemState(st->list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
  }
  sync_chrome_commands(st);
}

HTREEITEM insert_tree_recursive(ConsoleState* st, HWND tree, HTREEITEM parent, int node_id) {
  const BookNode* n = address_book_find(node_id);
  // The left tree shows the category tree only; agent/device entries are
  // listed in the detail list when a category is selected.
  if (!n || n->kind != BookNodeKind::kGroup) {
    return nullptr;
  }
  bool has_group_child = false;
  for (const BookNode* c : address_book_children(n->id)) {
    if (c->kind == BookNodeKind::kGroup) {
      has_group_child = true;
      break;
    }
  }
  TVINSERTSTRUCTW ins{};
  ins.hParent = parent;
  ins.hInsertAfter = TVI_LAST;
  ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
  ins.item.pszText = const_cast<wchar_t*>(n->name.c_str());
  ins.item.lParam = n->id;
  ins.item.cChildren = has_group_child ? 1 : 0;
  HTREEITEM h = TreeView_InsertItem(tree, &ins);
  for (const BookNode* c : address_book_children(n->id)) {
    if (c->kind == BookNodeKind::kGroup) {
      insert_tree_recursive(st, tree, h, c->id);
    }
  }
  return h;
}

void fill_tree(ConsoleState* st) {
  TreeView_DeleteAllItems(st->tree);
  for (const BookNode* n : address_book_children(-1)) {
    HTREEITEM root = insert_tree_recursive(st, st->tree, TVI_ROOT, n->id);
    TreeView_Expand(st->tree, root, TVE_EXPAND);
  }
  st->selected_group_id = 0;
  fill_list(st);
}

int selected_list_device_id(ConsoleState* st) {
  if (!st || !st->list) {
    return -1;
  }
  const int i = ListView_GetNextItem(st->list, -1, LVNI_SELECTED);
  if (i < 0) {
    return -1;
  }
  LVITEMW it{};
  it.mask = LVIF_PARAM;
  it.iItem = i;
  if (!ListView_GetItem(st->list, &it)) {
    return -1;
  }
  const BookNode* n = address_book_find(static_cast<int>(it.lParam));
  if (!n || n->kind != BookNodeKind::kDevice) {
    return -1;
  }
  return n->id;
}

// Prefer start when a list device is selected and not yet connected.
bool session_toolbar_starts(ConsoleState* st) {
  const int id = selected_list_device_id(st);
  return id >= 0 && find_session_by_device(st, id) < 0;
}

void sync_chrome_commands(ConsoleState* st) {
  if (!st) {
    return;
  }
  const bool has_sessions = !st->sessions.empty();
  const bool has_active = st->active_tab >= 0 &&
                          st->active_tab < static_cast<int>(st->sessions.size());
  const bool can_start = session_toolbar_starts(st);
  const bool has_list_sel = selected_list_device_id(st) >= 0;
  const bool can_session_btn = can_start || has_active || has_list_sel;

  if (st->toolbar) {
    SendMessageW(st->toolbar, TB_ENABLEBUTTON, kCmdSession, can_session_btn ? TRUE : FALSE);
    SendMessageW(st->toolbar, TB_CHANGEBITMAP, kCmdSession,
                 can_start ? kTbImgStart : kTbImgClose);
    SendMessageW(st->toolbar, TB_ENABLEBUTTON, kCmdCloseAll, has_sessions ? TRUE : FALSE);
    SendMessageW(st->toolbar, TB_ENABLEBUTTON, kCmdScreenshot, has_active ? TRUE : FALSE);
    SendMessageW(st->toolbar, TB_ENABLEBUTTON, kCmdFullscreen, has_active ? TRUE : FALSE);
    const bool view_only =
        has_active && st->sessions[static_cast<size_t>(st->active_tab)].host &&
        st->sessions[static_cast<size_t>(st->active_tab)].host->view_only();
    SendMessageW(st->toolbar, TB_ENABLEBUTTON, kCmdViewOnly, has_active ? TRUE : FALSE);
    SendMessageW(st->toolbar, TB_CHECKBUTTON, kCmdViewOnly, view_only ? TRUE : FALSE);
  }

  if (HMENU menu = st->hwnd ? GetMenu(st->hwnd) : nullptr) {
    auto set_item = [menu](UINT id, bool on) {
      EnableMenuItem(menu, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED));
    };
    set_item(kCmdRefresh, true);
    set_item(kCmdExit, true);
    set_item(kCmdAbout, true);
    set_item(kCmdStartSession, can_start || has_list_sel);
    set_item(kCmdCloseSession, has_active);
    set_item(kCmdCloseAll, has_sessions);
    set_item(kCmdNextTab, has_sessions);
    set_item(kCmdPrevTab, has_sessions);
    set_item(kCmdCyclePane, true);
  }
}

void refresh_directory(ConsoleState* st) {
  if (!st) {
    return;
  }
  if (address_book_source() == AddressBookSource::kDemo) {
    address_book_load(AddressBookSource::kDemo, {}, nullptr);
    fill_tree(st);
    set_status(st, L"目录已刷新（演示模式）");
    return;
  }
  DirectoryConfig dir = audit_directory_copy();
  if (dir.gateway_url.empty()) {
    dir = load_directory_config();
  }
  if (dir.directory_key.empty() && !dir.viewer_psk.empty()) {
    dir.directory_key = dir.viewer_psk;
  }
  std::string err;
  if (dir.gateway_url.empty() || dir.directory_key.empty() ||
      !address_book_load(AddressBookSource::kGateway, dir, &err)) {
    std::wstring msg = L"目录刷新失败：" + utf8_to_wide_local(err);
    set_status(st, msg.c_str());
    return;
  }
  audit_set_directory(dir);
  fill_tree(st);
  set_status(st, L"目录已刷新");
}

int find_session_by_device(ConsoleState* st, int device_id) {
  for (int i = 0; i < static_cast<int>(st->sessions.size()); ++i) {
    if (st->sessions[static_cast<size_t>(i)].device_id == device_id) {
      return i;
    }
  }
  return -1;
}

void ensure_tab_visible(ConsoleState* st, int tab_index);

void select_tab(ConsoleState* st, int tab_index) {
  st->active_tab = tab_index;
  if (tab_index >= 0 && tab_index < static_cast<int>(st->sessions.size())) {
    SessionTab& tab = st->sessions[static_cast<size_t>(tab_index)];
    if (tab.host && !tab.floating) {
      SessionHost::set_keyboard_target(tab.host.get());
    }
  } else {
    SessionHost::set_keyboard_target(nullptr);
  }
  layout(st);
  ensure_tab_visible(st, tab_index);
  if (st->tab_strip) {
    InvalidateRect(st->tab_strip, nullptr, TRUE);
  }
}

void focus_after_tab_change(ConsoleState* st) {
  if (!st) {
    return;
  }
  if (st->active_tab == kTabCatalog) {
    if (st->list && IsWindowVisible(st->list)) {
      ensure_list_selection(st);
      SetFocus(st->list);
    } else if (st->tree) {
      SetFocus(st->tree);
    }
  } else if (st->tab_strip && IsWindowVisible(st->tab_strip)) {
    SetFocus(st->tab_strip);
  }
}

void cmd_next_tab(ConsoleState* st) {
  if (!st || st->sessions.empty()) {
    return;
  }
  int next = kTabCatalog;
  if (st->active_tab == kTabCatalog) {
    next = 0;
  } else if (st->active_tab + 1 < static_cast<int>(st->sessions.size())) {
    next = st->active_tab + 1;
  } else {
    next = kTabCatalog;
  }
  select_tab(st, next);
  focus_after_tab_change(st);
}

void cmd_prev_tab(ConsoleState* st) {
  if (!st || st->sessions.empty()) {
    return;
  }
  int prev = kTabCatalog;
  if (st->active_tab == kTabCatalog) {
    prev = static_cast<int>(st->sessions.size()) - 1;
  } else if (st->active_tab <= 0) {
    prev = kTabCatalog;
  } else {
    prev = st->active_tab - 1;
  }
  select_tab(st, prev);
  focus_after_tab_change(st);
}

void cmd_cycle_pane(ConsoleState* st) {
  if (!st) {
    return;
  }
  const HWND focus = GetFocus();
  const bool list_ok = st->list && IsWindowVisible(st->list);
  const bool tabs_ok = st->tab_strip && IsWindowVisible(st->tab_strip);
  if (focus == st->tree) {
    if (list_ok) {
      ensure_list_selection(st);
      SetFocus(st->list);
    } else if (tabs_ok) {
      SetFocus(st->tab_strip);
    }
  } else if (focus == st->list) {
    if (tabs_ok) {
      SetFocus(st->tab_strip);
    } else if (st->tree) {
      SetFocus(st->tree);
    }
  } else if (focus == st->tab_strip) {
    if (st->tree) {
      SetFocus(st->tree);
    }
  } else if (st->tree) {
    SetFocus(st->tree);
  }
}

void remove_session_at(ConsoleState* st, int index) {
  if (index < 0 || index >= static_cast<int>(st->sessions.size())) {
    return;
  }
  st->sessions.erase(st->sessions.begin() + index);
  if (st->sessions.empty()) {
    st->active_tab = kTabCatalog;
  } else if (st->active_tab == index) {
    st->active_tab = (index > 0) ? index - 1 : 0;
  } else if (st->active_tab > index) {
    st->active_tab -= 1;
  }
  select_tab(st, st->active_tab);
  set_status(st, catalog_ready_status());
}

void on_session_closed(SessionHost* host) {
  ConsoleState* st = g_console;
  if (!st || !host || !st->hwnd) {
    return;
  }
  // Defer unique_ptr reset until after WM_DESTROY returns.
  PostMessageW(st->hwnd, WM_SESSION_CLOSED, 0, reinterpret_cast<LPARAM>(host));
}

bool open_session_for_device(ConsoleState* st, int device_id) {
  const BookNode* d = address_book_find(device_id);
  if (!d || d->kind != BookNodeKind::kDevice) {
    return false;
  }
  const int existing = find_session_by_device(st, device_id);
  if (existing >= 0) {
    SessionTab& tab = st->sessions[static_cast<size_t>(existing)];
    if (tab.floating && tab.host && tab.host->hwnd()) {
      SetForegroundWindow(tab.host->hwnd());
    } else {
      select_tab(st, existing);
    }
    return true;
  }
  if (static_cast<int>(st->sessions.size()) >= kMaxSessions) {
    set_status(st, L"已达会话上限 (8)");
    return false;
  }

  ConnectDefaults connect = st->connect;
  if (!d->host.empty()) {
    char hp[128];
    std::snprintf(hp, sizeof(hp), "%s:%d", d->host.c_str(), d->port > 0 ? d->port : 38471);
    connect.host_port = hp;
  }

  SessionTab tab;
  tab.device_id = device_id;
  tab.title = d->name;
  tab.host = std::make_unique<SessionHost>();
  tab.host->set_device_id(device_id);
  SessionHost* raw = tab.host.get();
  if (address_book_source() == AddressBookSource::kGateway && audit_reporting_enabled()) {
    std::string agent_name;
    // UTF-8 display name for audit (gateway expects UTF-8 JSON).
    {
      int n = WideCharToMultiByte(CP_UTF8, 0, d->name.c_str(), -1, nullptr, 0, nullptr, nullptr);
      if (n > 1) {
        agent_name.assign(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, d->name.c_str(), -1, agent_name.data(), n, nullptr, nullptr);
      }
    }
    raw->set_audit_session(audit_new_session_id(), d->agent_id, agent_name, connect.host_port);
  }
  if (!raw->open(st->instance, st->session_area, tab.title, connect,
                 [](SessionHost* h) { on_session_closed(h); },
                 /*auto_reconnect=*/true)) {
    set_status(st, L"连接失败");
    return false;
  }
  st->sessions.push_back(std::move(tab));
  select_tab(st, static_cast<int>(st->sessions.size()) - 1);
  if (raw->host_forced_view_only()) {
    set_status(st, L"已连接（仅观看：被控端已有控制端）");
  } else {
    set_status(st, address_book_source() == AddressBookSource::kGateway ? L"已连接（网关目录）"
                                                                       : L"已连接（演示：统一 Host Agent）");
  }
  return true;
}

std::wstring session_tab_label(const SessionTab& tab) {
  std::wstring label = tab.title;
  if (tab.host) {
    const std::string av = tab.host->agent_version();
    if (!av.empty()) {
      label += L" [";
      label += utf8_to_wide_local(av);
      label += L"]";
    }
  }
  return label;
}

int catalog_tab_width(HDC hdc) {
  SIZE sz{};
  GetTextExtentPoint32W(hdc, L"目录", 2, &sz);
  return sz.cx + 24;
}

int session_tab_width(HDC hdc, const SessionTab& tab, ConsoleState* st) {
  const std::wstring label = session_tab_label(tab);
  SIZE sz{};
  GetTextExtentPoint32W(hdc, label.c_str(), static_cast<int>(label.size()), &sz);
  int w = sz.cx + 16 + (st ? st->close_btn_w : kCloseBtnWDip);
  if (tab.floating && st) {
    w += st->float_badge_w + 4;
  }
  return w;
}

int tabs_content_width(ConsoleState* st, HDC hdc) {
  if (!st) {
    return 0;
  }
  int w = catalog_tab_width(hdc);
  for (const SessionTab& tab : st->sessions) {
    w += session_tab_width(hdc, tab, st);
  }
  return w;
}

bool tab_strip_needs_nav(ConsoleState* st, int strip_w, HDC hdc) {
  return tabs_content_width(st, hdc) > strip_w;
}

void clamp_tab_scroll(ConsoleState* st, int strip_w, HDC hdc) {
  if (!st) {
    return;
  }
  const bool nav = tab_strip_needs_nav(st, strip_w, hdc);
  const int vp = strip_w - (nav ? 2 * st->tab_nav_w : 0);
  const int content = tabs_content_width(st, hdc);
  const int max_scroll = (std::max)(0, content - (std::max)(0, vp));
  if (st->tab_scroll_x < 0) {
    st->tab_scroll_x = 0;
  }
  if (st->tab_scroll_x > max_scroll) {
    st->tab_scroll_x = max_scroll;
  }
}

void ensure_tab_visible(ConsoleState* st, int tab_index) {
  if (!st || !st->tab_strip) {
    return;
  }
  RECT rc{};
  GetClientRect(st->tab_strip, &rc);
  const int strip_w = rc.right - rc.left;
  HDC hdc = GetDC(st->tab_strip);
  HGDIOBJ old = st->ui_font ? SelectObject(hdc, st->ui_font) : nullptr;
  clamp_tab_scroll(st, strip_w, hdc);
  const bool nav = tab_strip_needs_nav(st, strip_w, hdc);
  const int vp_left = nav ? st->tab_nav_w : 0;
  const int vp_right = strip_w - (nav ? st->tab_nav_w : 0);
  const int vp_w = (std::max)(0, vp_right - vp_left);

  int tab_l = 4;
  int tab_r = tab_l;
  if (tab_index == kTabCatalog) {
    tab_r = tab_l + catalog_tab_width(hdc);
  } else if (tab_index >= 0 && tab_index < static_cast<int>(st->sessions.size())) {
    tab_l += catalog_tab_width(hdc);
    for (int i = 0; i < tab_index; ++i) {
      tab_l += session_tab_width(hdc, st->sessions[static_cast<size_t>(i)], st);
    }
    tab_r = tab_l + session_tab_width(hdc, st->sessions[static_cast<size_t>(tab_index)], st);
  } else {
    if (old) {
      SelectObject(hdc, old);
    }
    ReleaseDC(st->tab_strip, hdc);
    return;
  }

  const int vis_l = st->tab_scroll_x;
  const int vis_r = st->tab_scroll_x + vp_w;
  if (tab_l < vis_l) {
    st->tab_scroll_x = tab_l;
  } else if (tab_r > vis_r) {
    st->tab_scroll_x = tab_r - vp_w;
  }
  clamp_tab_scroll(st, strip_w, hdc);
  if (old) {
    SelectObject(hdc, old);
  }
  ReleaseDC(st->tab_strip, hdc);
  InvalidateRect(st->tab_strip, nullptr, FALSE);
}

// Returns kTabCatalog, session index, kTabHitClose/Nav*, or -2 (miss).
int tab_hit_test(ConsoleState* st, int x, int* out_close_index, RECT* out_tab_rc) {
  if (out_close_index) {
    *out_close_index = -1;
  }
  if (!st || !st->tab_strip) {
    return -2;
  }
  RECT rc{};
  GetClientRect(st->tab_strip, &rc);
  const int strip_w = rc.right - rc.left;
  HDC hdc = GetDC(st->tab_strip);
  HGDIOBJ old = st->ui_font ? SelectObject(hdc, st->ui_font) : nullptr;
  clamp_tab_scroll(st, strip_w, hdc);
  const bool nav = tab_strip_needs_nav(st, strip_w, hdc);
  if (nav) {
    if (x >= 0 && x < st->tab_nav_w) {
      if (old) {
        SelectObject(hdc, old);
      }
      ReleaseDC(st->tab_strip, hdc);
      return kTabHitNavLeft;
    }
    if (x >= strip_w - st->tab_nav_w && x < strip_w) {
      if (old) {
        SelectObject(hdc, old);
      }
      ReleaseDC(st->tab_strip, hdc);
      return kTabHitNavRight;
    }
  }

  const int vp_left = nav ? st->tab_nav_w : 0;
  const int content_x = x - vp_left + st->tab_scroll_x;
  int cx = 4;
  const int cat_w = catalog_tab_width(hdc);
  if (content_x >= cx && content_x < cx + cat_w) {
    if (out_tab_rc) {
      *out_tab_rc = {vp_left + cx - st->tab_scroll_x, 0,
                     vp_left + cx - st->tab_scroll_x + cat_w, st->tab_h};
    }
    if (old) {
      SelectObject(hdc, old);
    }
    ReleaseDC(st->tab_strip, hdc);
    return kTabCatalog;
  }
  cx += cat_w;
  for (int i = 0; i < static_cast<int>(st->sessions.size()); ++i) {
    const SessionTab& tab = st->sessions[static_cast<size_t>(i)];
    const int tw = session_tab_width(hdc, tab, st);
    if (content_x >= cx && content_x < cx + tw) {
      if (out_tab_rc) {
        *out_tab_rc = {vp_left + cx - st->tab_scroll_x, 0,
                       vp_left + cx - st->tab_scroll_x + tw, st->tab_h};
      }
      const int close_left = cx + tw - st->close_btn_w - 2;
      if (out_close_index && content_x >= close_left) {
        *out_close_index = i;
        if (old) {
          SelectObject(hdc, old);
        }
        ReleaseDC(st->tab_strip, hdc);
        return kTabHitClose;
      }
      if (old) {
        SelectObject(hdc, old);
      }
      ReleaseDC(st->tab_strip, hdc);
      return i;
    }
    cx += tw;
  }
  if (old) {
    SelectObject(hdc, old);
  }
  ReleaseDC(st->tab_strip, hdc);
  return -2;
}

void paint_tab_nav_btn(HDC hdc, const RECT& rc, bool enabled, bool left, int dpi) {
  if (enabled) {
    fill_round_rect(hdc, rc, kPanel, kBorderSubtle, dip(4, dpi) * 2);
  }
  const COLORREF ink = enabled ? kTextSecondary : kTextMuted;
  HPEN pen = CreatePen(PS_SOLID, 2, ink);
  HGDIOBJ old = SelectObject(hdc, pen);
  const int cx = (rc.left + rc.right) / 2;
  const int cy = (rc.top + rc.bottom) / 2;
  if (left) {
    MoveToEx(hdc, cx + 3, cy - 5, nullptr);
    LineTo(hdc, cx - 2, cy);
    LineTo(hdc, cx + 3, cy + 5);
  } else {
    MoveToEx(hdc, cx - 3, cy - 5, nullptr);
    LineTo(hdc, cx + 2, cy);
    LineTo(hdc, cx - 3, cy + 5);
  }
  SelectObject(hdc, old);
  DeleteObject(pen);
}

// Lucide-ish panel-top-open mark for torn-out session tabs.
void paint_float_badge(HDC hdc, int left, int top, int size) {
  if (size < 8) {
    return;
  }
  HPEN pen = CreatePen(PS_SOLID, 1, kAccent);
  HBRUSH br = CreateSolidBrush(kAccent);
  HGDIOBJ old_pen = SelectObject(hdc, pen);
  HGDIOBJ old_br = SelectObject(hdc, GetStockObject(NULL_BRUSH));
  const int r = size;
  RoundRect(hdc, left, top, left + r, top + r * 3 / 4, 2, 2);
  SelectObject(hdc, br);
  Rectangle(hdc, left, top, left + r, top + r / 4);
  SelectObject(hdc, old_pen);
  SelectObject(hdc, old_br);
  DeleteObject(pen);
  DeleteObject(br);
}

void paint_tab_strip(HWND hwnd, ConsoleState* st) {
  PAINTSTRUCT ps{};
  HDC hdc = BeginPaint(hwnd, &ps);
  RECT rc{};
  GetClientRect(hwnd, &rc);
  HBRUSH chrome = CreateSolidBrush(kChrome);
  FillRect(hdc, &rc, chrome);
  DeleteObject(chrome);
  HPEN edge = CreatePen(PS_SOLID, 1, kBorderSubtle);
  HGDIOBJ old_pen = SelectObject(hdc, edge);
  MoveToEx(hdc, rc.left, rc.bottom - 1, nullptr);
  LineTo(hdc, rc.right, rc.bottom - 1);
  SelectObject(hdc, old_pen);
  DeleteObject(edge);

  HGDIOBJ old_font = st->ui_font ? SelectObject(hdc, st->ui_font) : nullptr;
  SetBkMode(hdc, TRANSPARENT);
  const int strip_w = rc.right - rc.left;
  clamp_tab_scroll(st, strip_w, hdc);
  const bool nav = tab_strip_needs_nav(st, strip_w, hdc);
  const int nav_w = st->tab_nav_w;
  const int vp_left = nav ? nav_w : 0;
  const int vp_right = strip_w - (nav ? nav_w : 0);
  const int diameter = dip(4, st->dpi) * 2;

  if (nav) {
    const int max_scroll =
        (std::max)(0, tabs_content_width(st, hdc) - (vp_right - vp_left));
    const int ny = (rc.bottom - nav_w) / 2;
    RECT left_rc{2, ny, 2 + nav_w, ny + nav_w};
    RECT right_rc{strip_w - nav_w - 2, ny, strip_w - 2, ny + nav_w};
    paint_tab_nav_btn(hdc, left_rc, st->tab_scroll_x > 0, true, st->dpi);
    paint_tab_nav_btn(hdc, right_rc, st->tab_scroll_x < max_scroll, false, st->dpi);
  }

  HRGN clip = CreateRectRgn(vp_left, 0, vp_right, rc.bottom);
  SelectClipRgn(hdc, clip);

  auto draw_catalog = [&](int x) {
    const int w = catalog_tab_width(hdc);
    RECT tr{x, 2, x + w, rc.bottom - 1};
    const bool active = st->active_tab == kTabCatalog;
    fill_round_rect(hdc, tr, active ? kTabActive : kTabInactive,
                    active ? kTabActive : kTabInactive, diameter);
    if (active) {
      HPEN ap = CreatePen(PS_SOLID, 2, kAccent);
      HGDIOBJ op = SelectObject(hdc, ap);
      MoveToEx(hdc, tr.left + 2, 2, nullptr);
      LineTo(hdc, tr.right - 2, 2);
      SelectObject(hdc, op);
      DeleteObject(ap);
    }
    SetTextColor(hdc, active ? kTextPrimary : kTextSecondary);
    DrawTextW(hdc, L"目录", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return w;
  };

  auto draw_session = [&](int x, int index) {
    const SessionTab& tab = st->sessions[static_cast<size_t>(index)];
    const std::wstring label = session_tab_label(tab);
    const int w = session_tab_width(hdc, tab, st);
    RECT tr{x, 2, x + w, rc.bottom - 1};
    const bool active = st->active_tab == index;
    fill_round_rect(hdc, tr, active ? kTabActive : kTabInactive,
                    active ? kTabActive : kTabInactive, diameter);
    if (active) {
      HPEN ap = CreatePen(PS_SOLID, 2, kAccent);
      HGDIOBJ op = SelectObject(hdc, ap);
      MoveToEx(hdc, tr.left + 2, 2, nullptr);
      LineTo(hdc, tr.right - 2, 2);
      SelectObject(hdc, op);
      DeleteObject(ap);
    }
    RECT text_rc = tr;
    text_rc.right -= st->close_btn_w;
    if (tab.floating) {
      text_rc.right -= st->float_badge_w + 4;
      const int bx = tr.right - st->close_btn_w - st->float_badge_w - 4;
      const int by = tr.top + (tr.bottom - tr.top - st->float_badge_w * 3 / 4) / 2;
      paint_float_badge(hdc, bx, by, st->float_badge_w);
    }
    SetTextColor(hdc, active ? kTextPrimary : kTextSecondary);
    DrawTextW(hdc, label.c_str(), -1, &text_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT close_rc{tr.right - st->close_btn_w - 2, tr.top, tr.right - 2, tr.bottom};
    SetTextColor(hdc, kTextMuted);
    DrawTextW(hdc, L"×", -1, &close_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return w;
  };

  int cx = vp_left + 4 - st->tab_scroll_x;
  cx += draw_catalog(cx);
  for (int i = 0; i < static_cast<int>(st->sessions.size()); ++i) {
    cx += draw_session(cx, i);
  }

  SelectClipRgn(hdc, nullptr);
  DeleteObject(clip);
  if (GetFocus() == hwnd) {
    RECT fr = rc;
    InflateRect(&fr, -1, -1);
    DrawFocusRect(hdc, &fr);
  }
  if (old_font) {
    SelectObject(hdc, old_font);
  }
  EndPaint(hwnd, &ps);
}

void close_session_at(ConsoleState* st, int index);  // defined below
void close_all_sessions(ConsoleState* st);

enum class CtxIcon : int {
  Close = 0,
  CloseOthers,
  CloseAll,
  Tear,
  Dock,
  Fullscreen,
  Eye,
  Count,
};

HBITMAP make_ctx_menu_bitmap(CtxIcon which, int icon_px, COLORREF ink) {
  if (icon_px < 12) {
    icon_px = 12;
  }
  ULONG_PTR token = 0;
  gdip::GdiplusStartupInput gsi;
  if (gdip::GdiplusStartup(&token, &gsi, nullptr) != gdip::Ok) {
    return nullptr;
  }
  HBITMAP hbmp = nullptr;
  {
    gdip::Bitmap bmp(icon_px, icon_px, PixelFormat32bppPARGB);
    gdip::Graphics g(&bmp);
    g.SetSmoothingMode(gdip::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(gdip::PixelOffsetModeHalf);
    g.Clear(gdip::Color(0, 0, 0, 0));
    constexpr float kMargin = 1.5f;
    const float inner = static_cast<float>(icon_px) - 2.f * kMargin;
    const float scale = inner / 24.f;
    g.TranslateTransform(kMargin, kMargin);
    g.ScaleTransform(scale, scale);
    gdip::Pen pen(gdip_rgb(ink), 2.f);
    pen.SetStartCap(gdip::LineCapRound);
    pen.SetEndCap(gdip::LineCapRound);
    pen.SetLineJoin(gdip::LineJoinRound);

    switch (which) {
      case CtxIcon::Close:
        g.DrawLine(&pen, 18.f, 6.f, 6.f, 18.f);
        g.DrawLine(&pen, 6.f, 6.f, 18.f, 18.f);
        break;
      case CtxIcon::CloseOthers:
        // lucide copy-x — stacked frames + x
        g.DrawRectangle(&pen, 8.f, 8.f, 12.f, 12.f);
        g.DrawRectangle(&pen, 4.f, 4.f, 12.f, 12.f);
        g.DrawLine(&pen, 7.f, 7.f, 13.f, 13.f);
        g.DrawLine(&pen, 13.f, 7.f, 7.f, 13.f);
        break;
      case CtxIcon::CloseAll:
        g.DrawEllipse(&pen, 3.f, 3.f, 18.f, 18.f);
        g.DrawLine(&pen, 15.f, 9.f, 9.f, 15.f);
        g.DrawLine(&pen, 9.f, 9.f, 15.f, 15.f);
        break;
      case CtxIcon::Tear:
        // lucide panel-top-open
        g.DrawRectangle(&pen, 3.f, 4.f, 18.f, 16.f);
        g.DrawLine(&pen, 3.f, 9.f, 21.f, 9.f);
        g.DrawLine(&pen, 12.f, 14.f, 12.f, 19.f);
        {
          gdip::PointF tip[] = {{9.f, 16.5f}, {12.f, 13.5f}, {15.f, 16.5f}};
          g.DrawLines(&pen, tip, 3);
        }
        break;
      case CtxIcon::Dock:
        // lucide panel-top-close
        g.DrawRectangle(&pen, 3.f, 4.f, 18.f, 16.f);
        g.DrawLine(&pen, 3.f, 9.f, 21.f, 9.f);
        g.DrawLine(&pen, 12.f, 12.f, 12.f, 17.f);
        {
          gdip::PointF tip[] = {{9.f, 14.5f}, {12.f, 17.5f}, {15.f, 14.5f}};
          g.DrawLines(&pen, tip, 3);
        }
        break;
      case CtxIcon::Fullscreen: {
        gdip::PointF tl[] = {{8.f, 3.f}, {5.f, 3.f}, {5.f, 6.f}};
        gdip::PointF tr[] = {{16.f, 3.f}, {19.f, 3.f}, {19.f, 6.f}};
        gdip::PointF bl[] = {{5.f, 16.f}, {5.f, 19.f}, {8.f, 19.f}};
        gdip::PointF br[] = {{19.f, 16.f}, {19.f, 19.f}, {16.f, 19.f}};
        g.DrawLines(&pen, tl, 3);
        g.DrawLines(&pen, tr, 3);
        g.DrawLines(&pen, bl, 3);
        g.DrawLines(&pen, br, 3);
        break;
      }
      case CtxIcon::Eye: {
        gdip::GraphicsPath eye;
        eye.AddBezier(2.f, 12.f, 6.f, 6.5f, 18.f, 6.5f, 22.f, 12.f);
        eye.AddBezier(22.f, 12.f, 18.f, 17.5f, 6.f, 17.5f, 2.f, 12.f);
        g.DrawPath(&pen, &eye);
        g.DrawEllipse(&pen, 9.f, 9.f, 6.f, 6.f);
        break;
      }
      default:
        break;
    }
    bmp.GetHBITMAP(gdip::Color(0, 0, 0, 0), &hbmp);
  }  // Bitmap/Graphics destroyed before GdiplusShutdown
  gdip::GdiplusShutdown(token);
  return hbmp;
}

void append_ctx_menu_item(HMENU menu, UINT id, const wchar_t* text, HBITMAP bmp, bool enabled) {
  MENUITEMINFOW mii{};
  mii.cbSize = sizeof(mii);
  mii.fMask = MIIM_ID | MIIM_STRING | MIIM_BITMAP | MIIM_STATE | MIIM_FTYPE;
  mii.fType = MFT_STRING;
  mii.fState = enabled ? MFS_ENABLED : (MFS_DISABLED | MFS_GRAYED);
  mii.wID = id;
  mii.dwTypeData = const_cast<LPWSTR>(text);
  mii.cch = static_cast<UINT>(wcslen(text));
  mii.hbmpItem = bmp;
  InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &mii);
}

void run_tab_context_menu(ConsoleState* st, HWND hwnd, int tab_index, int screen_x, int screen_y) {
  if (!st || tab_index < 0 || tab_index >= static_cast<int>(st->sessions.size())) {
    return;
  }
  SessionTab& tab = st->sessions[static_cast<size_t>(tab_index)];
  const int icon_px = dip(14, st->dpi);
  HBITMAP bmps[static_cast<int>(CtxIcon::Count)] = {};
  auto bmp_for = [&](CtxIcon ic, bool enabled) -> HBITMAP {
    const int idx = static_cast<int>(ic);
    if (!bmps[idx]) {
      bmps[idx] = make_ctx_menu_bitmap(ic, icon_px, enabled ? kTextSecondary : kTextMuted);
    }
    return bmps[idx];
  };

  HMENU menu = CreatePopupMenu();
  // Optional title row (design Frame C header).
  {
    std::wstring hdr = tab.title;
    if (tab.floating) {
      hdr += L" · 已拖出";
    }
    MENUITEMINFOW mii{};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING | MIIM_STATE | MIIM_FTYPE | MIIM_ID;
    mii.fType = MFT_STRING;
    mii.fState = MFS_DISABLED;
    mii.wID = 0;
    mii.dwTypeData = hdr.data();
    mii.cch = static_cast<UINT>(hdr.size());
    InsertMenuItemW(menu, 0, TRUE, &mii);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  }
  append_ctx_menu_item(menu, kCmdTabCtxClose, L"关闭", bmp_for(CtxIcon::Close, true), true);
  append_ctx_menu_item(menu, kCmdTabCtxCloseOthers, L"关闭其他", bmp_for(CtxIcon::CloseOthers, true),
                       true);
  append_ctx_menu_item(menu, kCmdTabCtxCloseAll, L"关闭全部", bmp_for(CtxIcon::CloseAll, true), true);
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  append_ctx_menu_item(menu, kCmdTabCtxTear, L"拖出为独立窗口",
                       bmp_for(CtxIcon::Tear, !tab.floating), !tab.floating);
  append_ctx_menu_item(menu, kCmdTabCtxDock, L"拖回工作区", bmp_for(CtxIcon::Dock, tab.floating),
                       tab.floating);
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  append_ctx_menu_item(menu, kCmdTabCtxFullscreen, L"全屏", bmp_for(CtxIcon::Fullscreen, true),
                       true);
  append_ctx_menu_item(menu, kCmdTabCtxViewOnly, L"只读", bmp_for(CtxIcon::Eye, true), true);

  const int cmd =
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_x, screen_y, 0, hwnd, nullptr);
  DestroyMenu(menu);
  for (HBITMAP b : bmps) {
    if (b) {
      DeleteObject(b);
    }
  }
  if (!cmd) {
    return;
  }

  SessionHost* host = tab.host.get();
  switch (cmd) {
    case kCmdTabCtxClose:
      close_session_at(st, tab_index);
      break;
    case kCmdTabCtxCloseOthers: {
      std::vector<SessionHost*> victims;
      for (int i = 0; i < static_cast<int>(st->sessions.size()); ++i) {
        if (i == tab_index) {
          continue;
        }
        if (st->sessions[static_cast<size_t>(i)].host) {
          victims.push_back(st->sessions[static_cast<size_t>(i)].host.get());
        }
      }
      for (SessionHost* h : victims) {
        if (h) {
          h->close();
        }
      }
      break;
    }
    case kCmdTabCtxCloseAll:
      close_all_sessions(st);
      break;
    case kCmdTabCtxTear:
      if (host && !tab.floating) {
        host->detach_to_floating(st->instance);
        tab.floating = true;
        select_tab(st, kTabCatalog);
        SessionHost::set_keyboard_target(host);
        set_status(st, L"会话已拖出为独立窗口");
        InvalidateRect(st->tab_strip, nullptr, FALSE);
      }
      break;
    case kCmdTabCtxDock:
      if (host && tab.floating) {
        host->attach_to_parent(st->session_area);
        tab.floating = false;
        select_tab(st, tab_index);
        set_status(st, L"会话已拖回工作区");
        InvalidateRect(st->tab_strip, nullptr, FALSE);
      }
      break;
    case kCmdTabCtxFullscreen:
      if (host) {
        host->toggle_fullscreen();
      }
      break;
    case kCmdTabCtxViewOnly:
      if (host) {
        if (host->host_forced_view_only()) {
          set_status(st, L"被控端已有控制端，本会话仅观看");
        } else {
          host->set_view_only(!host->view_only());
          sync_chrome_commands(st);
          set_status(st, host->view_only() ? L"仅查看模式：键鼠输入已冻结" : L"已恢复键鼠控制");
        }
      }
      break;
    default:
      break;
  }
}

void close_session_at(ConsoleState* st, int index) {
  if (!st || index < 0 || index >= static_cast<int>(st->sessions.size())) {
    return;
  }
  SessionTab& tab = st->sessions[static_cast<size_t>(index)];
  if (tab.host) {
    // close() → DestroyWindow → WM_SESSION_CLOSED removes the tab.
    tab.host->close();
  } else {
    remove_session_at(st, index);
  }
  set_status(st, L"已断开连接");
}

void close_all_sessions(ConsoleState* st) {
  if (!st) {
    return;
  }
  for (SessionTab& tab : st->sessions) {
    if (tab.host) {
      tab.host->close();
    }
  }
  set_status(st, L"已断开全部会话");
}

std::wstring default_screenshot_path(const std::wstring& title) {
  wchar_t dir[MAX_PATH] = {};
  if (SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, dir) != S_OK) {
    GetTempPathW(MAX_PATH, dir);
  }
  std::wstring safe = title;
  for (wchar_t& ch : safe) {
    if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' ||
        ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
      ch = L'_';
    }
  }
  SYSTEMTIME now{};
  GetLocalTime(&now);
  wchar_t name[96];
  _snwprintf_s(name, _TRUNCATE, L"RoadDesk_%s_%04d%02d%02d_%02d%02d%02d.png", safe.c_str(),
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  return std::wstring(dir) + L"\\" + name;
}

HIMAGELIST build_toolbar_images(int icon_px, COLORREF ink) {
  if (icon_px < 12) {
    icon_px = 12;
  }

  ULONG_PTR gdip_token = 0;
  gdip::GdiplusStartupInput gsi;
  if (gdip::GdiplusStartup(&gdip_token, &gsi, nullptr) != gdip::Ok) {
    return nullptr;
  }

  HIMAGELIST il = ImageList_Create(icon_px, icon_px, ILC_COLOR32, 7, 1);
  if (!il) {
    gdip::GdiplusShutdown(gdip_token);
    return nullptr;
  }

  enum class TbIcon : int {
    Refresh = 0,
    Close,
    CloseAll,
    Camera,
    Maximize,
    Eye,
    Monitor,
  };

  auto draw_icon = [&](TbIcon which, gdip::Graphics* g) {
    g->SetSmoothingMode(gdip::SmoothingModeAntiAlias);
    g->SetPixelOffsetMode(gdip::PixelOffsetModeHalf);
    g->Clear(gdip::Color(0, 0, 0, 0));
    // Keep stroke caps inside the bitmap (stroke-width 2 on 24 viewBox).
    constexpr float kMargin = 1.5f;
    const float inner = static_cast<float>(icon_px) - 2.f * kMargin;
    const float scale = inner / 24.f;
    g->TranslateTransform(kMargin, kMargin);
    g->ScaleTransform(scale, scale);

    gdip::Pen pen(gdip_rgb(ink), 2.f);
    pen.SetStartCap(gdip::LineCapRound);
    pen.SetEndCap(gdip::LineCapRound);
    pen.SetLineJoin(gdip::LineJoinRound);
    pen.SetMiterLimit(2.f);

    switch (which) {
      case TbIcon::Refresh: {
        // lucide refresh-cw — both arcs + arrow tips, inset for stroke
        g->DrawArc(&pen, 4.f, 4.f, 16.f, 16.f, -50.f, 200.f);
        {
          gdip::PointF tip[] = {{16.5f, 4.f}, {20.f, 4.f}, {20.f, 7.5f}};
          g->DrawLines(&pen, tip, 3);
        }
        g->DrawArc(&pen, 4.f, 4.f, 16.f, 16.f, 130.f, 200.f);
        {
          gdip::PointF tip[] = {{7.5f, 20.f}, {4.f, 20.f}, {4.f, 16.5f}};
          g->DrawLines(&pen, tip, 3);
        }
        break;
      }
      case TbIcon::Close:
        g->DrawLine(&pen, 18.f, 6.f, 6.f, 18.f);
        g->DrawLine(&pen, 6.f, 6.f, 18.f, 18.f);
        break;
      case TbIcon::CloseAll:
        // lucide x-circle
        g->DrawEllipse(&pen, 3.f, 3.f, 18.f, 18.f);
        g->DrawLine(&pen, 15.f, 9.f, 9.f, 15.f);
        g->DrawLine(&pen, 9.f, 9.f, 15.f, 15.f);
        break;
      case TbIcon::Camera: {
        // lucide camera (body + lens)
        gdip::GraphicsPath body;
        body.StartFigure();
        body.AddLine(9.5f, 4.f, 7.f, 7.f);
        body.AddLine(7.f, 7.f, 4.f, 7.f);
        body.AddArc(2.f, 7.f, 4.f, 4.f, 270.f, -90.f);  // (4,7)->(2,9)
        body.AddLine(2.f, 9.f, 2.f, 18.f);
        body.AddArc(2.f, 16.f, 4.f, 4.f, 180.f, -90.f);  // (2,18)->(4,20)
        body.AddLine(4.f, 20.f, 20.f, 20.f);
        body.AddArc(18.f, 16.f, 4.f, 4.f, 90.f, -90.f);  // (20,20)->(22,18)
        body.AddLine(22.f, 18.f, 22.f, 9.f);
        body.AddArc(18.f, 7.f, 4.f, 4.f, 0.f, -90.f);  // (22,9)->(20,7)
        body.AddLine(20.f, 7.f, 17.f, 7.f);
        body.AddLine(17.f, 7.f, 14.5f, 4.f);
        body.AddLine(14.5f, 4.f, 9.5f, 4.f);
        g->DrawPath(&pen, &body);
        g->DrawEllipse(&pen, 9.f, 10.f, 6.f, 6.f);
        break;
      }
      case TbIcon::Maximize: {
        // lucide maximize / fullscreen corners
        gdip::PointF tl[] = {{8.f, 3.f}, {5.f, 3.f}, {5.f, 6.f}};
        gdip::PointF tr[] = {{16.f, 3.f}, {19.f, 3.f}, {19.f, 6.f}};
        gdip::PointF bl[] = {{5.f, 16.f}, {5.f, 19.f}, {8.f, 19.f}};
        gdip::PointF br[] = {{19.f, 16.f}, {19.f, 19.f}, {16.f, 19.f}};
        g->DrawLines(&pen, tl, 3);
        g->DrawLines(&pen, tr, 3);
        g->DrawLines(&pen, bl, 3);
        g->DrawLines(&pen, br, 3);
        break;
      }
      case TbIcon::Eye: {
        gdip::GraphicsPath eye;
        eye.AddBezier(2.f, 12.f, 6.f, 6.5f, 18.f, 6.5f, 22.f, 12.f);
        eye.AddBezier(22.f, 12.f, 18.f, 17.5f, 6.f, 17.5f, 2.f, 12.f);
        g->DrawPath(&pen, &eye);
        g->DrawEllipse(&pen, 9.f, 9.f, 6.f, 6.f);
        break;
      }
      case TbIcon::Monitor: {
        // lucide monitor — screen + stand (start session)
        g->DrawRectangle(&pen, 2.f, 3.f, 20.f, 14.f);
        g->DrawLine(&pen, 12.f, 17.f, 12.f, 21.f);
        g->DrawLine(&pen, 8.f, 21.f, 16.f, 21.f);
        break;
      }
    }
  };

  for (int i = 0; i < 7; ++i) {
    gdip::Bitmap bmp(icon_px, icon_px, PixelFormat32bppPARGB);
    gdip::Graphics g(&bmp);
    draw_icon(static_cast<TbIcon>(i), &g);
    HBITMAP hbmp = nullptr;
    if (bmp.GetHBITMAP(gdip::Color(0, 0, 0, 0), &hbmp) == gdip::Ok && hbmp) {
      ImageList_Add(il, hbmp, nullptr);
      DeleteObject(hbmp);
    }
  }

  gdip::GdiplusShutdown(gdip_token);
  return il;
}

LRESULT CALLBACK TabStripProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  ConsoleState* st = g_console;
  switch (msg) {
    case WM_GETDLGCODE:
      return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_KEYDOWN:
      if (!st) {
        break;
      }
      switch (wparam) {
        case VK_LEFT:
        case VK_UP:
          cmd_prev_tab(st);
          return 0;
        case VK_RIGHT:
        case VK_DOWN:
          cmd_next_tab(st);
          return 0;
        case VK_HOME:
          select_tab(st, kTabCatalog);
          focus_after_tab_change(st);
          return 0;
        case VK_END:
          if (!st->sessions.empty()) {
            select_tab(st, static_cast<int>(st->sessions.size()) - 1);
            focus_after_tab_change(st);
          }
          return 0;
        case VK_DELETE:
        case VK_BACK:
          if (st->active_tab >= 0 &&
              st->active_tab < static_cast<int>(st->sessions.size())) {
            close_session_at(st, st->active_tab);
          }
          return 0;
        case VK_RETURN:
        case VK_SPACE:
          // Active tab already selected; Space/Enter keep focus here.
          return 0;
        default:
          break;
      }
      break;
    case WM_PAINT:
      if (st) {
        paint_tab_strip(hwnd, st);
      }
      return 0;
    case WM_LBUTTONDBLCLK: {
      if (!st) {
        break;
      }
      st->dragging_tab = false;
      st->drag_tab_index = -1;
      st->drag_tear_armed = false;
      st->close_hit_index = -1;
      ReleaseCapture();
      int close_idx = -1;
      const int hit = tab_hit_test(st, GET_X_LPARAM(lparam), &close_idx, nullptr);
      if (hit >= 0 && hit < static_cast<int>(st->sessions.size())) {
        SessionHost* host = st->sessions[static_cast<size_t>(hit)].host.get();
        if (host) {
          host->toggle_fullscreen();
        }
      }
      return 0;
    }
    case WM_LBUTTONDOWN: {
      if (!st) {
        break;
      }
      const int x = GET_X_LPARAM(lparam);
      int close_idx = -1;
      const int hit = tab_hit_test(st, x, &close_idx, nullptr);
      if (hit == kTabHitNavLeft || hit == kTabHitNavRight) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int strip_w = rc.right - rc.left;
        HDC hdc = GetDC(hwnd);
        HGDIOBJ old = st->ui_font ? SelectObject(hdc, st->ui_font) : nullptr;
        const int step = catalog_tab_width(hdc) + 40;
        if (hit == kTabHitNavLeft) {
          st->tab_scroll_x -= step;
        } else {
          st->tab_scroll_x += step;
        }
        clamp_tab_scroll(st, strip_w, hdc);
        if (old) {
          SelectObject(hdc, old);
        }
        ReleaseDC(hwnd, hdc);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (hit == kTabHitClose && close_idx >= 0) {
        st->close_hit_index = close_idx;
        st->dragging_tab = false;
        return 0;
      }
      st->close_hit_index = -1;
      if (hit == kTabCatalog) {
        select_tab(st, kTabCatalog);
      } else if (hit >= 0) {
        select_tab(st, hit);
        st->dragging_tab = true;
        st->drag_tear_armed = false;
        st->drag_tab_index = hit;
        st->drag_start = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetCapture(hwnd);
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!st || !st->dragging_tab || st->drag_tab_index < 0) {
        break;
      }
      // Only arm tear-out while dragging; commit on LBUTTONUP.
      const int dx = GET_X_LPARAM(lparam) - st->drag_start.x;
      const int dy = GET_Y_LPARAM(lparam) - st->drag_start.y;
      if (dy < -24 || (dy * dy + dx * dx) > 36 * 36) {
        st->drag_tear_armed = true;
        SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
        if (st->drag_tab_index >= 0 &&
            st->drag_tab_index < static_cast<int>(st->sessions.size())) {
          ensure_drag_ghost(st, st->sessions[static_cast<size_t>(st->drag_tab_index)].title);
          POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
          ClientToScreen(hwnd, &pt);
          move_drag_ghost(st, pt);
        }
      } else if (st->drag_ghost) {
        // Pulled back into tab strip — cancel preview.
        destroy_drag_ghost(st);
        st->drag_tear_armed = false;
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      if (!st) {
        break;
      }
      if (st->close_hit_index >= 0) {
        int close_idx = -1;
        const int hit = tab_hit_test(st, GET_X_LPARAM(lparam), &close_idx, nullptr);
        const int pending = st->close_hit_index;
        st->close_hit_index = -1;
        if (hit == kTabHitClose && close_idx == pending) {
          close_session_at(st, pending);
        }
        return 0;
      }
      if (st->dragging_tab) {
        const int idx = st->drag_tab_index;
        const bool tear = st->drag_tear_armed;
        POINT release_pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ClientToScreen(hwnd, &release_pt);
        st->dragging_tab = false;
        st->drag_tear_armed = false;
        st->drag_tab_index = -1;
        destroy_drag_ghost(st);
        ReleaseCapture();
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        if (tear && idx >= 0 && idx < static_cast<int>(st->sessions.size())) {
          SessionTab& tab = st->sessions[static_cast<size_t>(idx)];
          if (tab.host && !tab.floating) {
            tab.host->detach_to_floating(st->instance);
            tab.floating = true;
            // Place real window near drop point.
            if (tab.host->hwnd()) {
              SetWindowPos(tab.host->hwnd(), nullptr, release_pt.x - 40, release_pt.y - 14,
                           0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);
            }
            SessionHost* floated = tab.host.get();
            select_tab(st, kTabCatalog);
            SessionHost::set_keyboard_target(floated);
            set_status(st, L"会话已拖出为独立窗口");
            InvalidateRect(st->tab_strip, nullptr, FALSE);
          }
        }
      }
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (st && reinterpret_cast<HWND>(lparam) != hwnd) {
        st->dragging_tab = false;
        st->drag_tear_armed = false;
        st->drag_tab_index = -1;
        st->close_hit_index = -1;
        destroy_drag_ghost(st);
      }
      return 0;
    case WM_RBUTTONUP: {
      if (!st) {
        break;
      }
      int close_idx = -1;
      const int hit = tab_hit_test(st, GET_X_LPARAM(lparam), &close_idx, nullptr);
      if (hit >= 0 && hit < static_cast<int>(st->sessions.size())) {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ClientToScreen(hwnd, &pt);
        run_tab_context_menu(st, hwnd, hit, pt.x, pt.y);
      }
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

HMENU build_console_menu() {
  HMENU menu = CreateMenu();
  HMENU file = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, kCmdExit, L"退出(&X)\tAlt+F4");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"文件(&F)");
  HMENU view = CreatePopupMenu();
  AppendMenuW(view, MF_STRING, kCmdRefresh, L"刷新(&R)\tF5");
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_STRING, kCmdNextTab, L"下一标签(&N)\tCtrl+Tab");
  AppendMenuW(view, MF_STRING, kCmdPrevTab, L"上一标签(&P)\tCtrl+Shift+Tab");
  AppendMenuW(view, MF_STRING, kCmdCyclePane, L"切换窗格(&G)\tF6");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"查看(&V)");
  HMENU session = CreatePopupMenu();
  AppendMenuW(session, MF_STRING, kCmdStartSession, L"启动会话(&O)\tCtrl+Enter");
  AppendMenuW(session, MF_STRING, kCmdCloseSession, L"关闭会话(&W)\tCtrl+W");
  AppendMenuW(session, MF_STRING, kCmdCloseAll, L"全部断开(&A)\tCtrl+Shift+W");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(session), L"会话(&S)");
  HMENU help = CreatePopupMenu();
  AppendMenuW(help, MF_STRING, kCmdAbout, L"关于(&A)\tF1");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"帮助(&H)");
  return menu;
}

enum class MenuBarIcon : int {
  Exit = 0,
  Refresh,
  Start,
  Close,
  CloseAll,
  About,
  Count,
};

HBITMAP make_menu_bar_bitmap(MenuBarIcon which, int icon_px, COLORREF ink) {
  if (icon_px < 12) {
    icon_px = 12;
  }
  ULONG_PTR token = 0;
  gdip::GdiplusStartupInput gsi;
  if (gdip::GdiplusStartup(&token, &gsi, nullptr) != gdip::Ok) {
    return nullptr;
  }
  HBITMAP hbmp = nullptr;
  {
    gdip::Bitmap bmp(icon_px, icon_px, PixelFormat32bppPARGB);
    gdip::Graphics g(&bmp);
    g.SetSmoothingMode(gdip::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(gdip::PixelOffsetModeHalf);
    g.Clear(gdip::Color(0, 0, 0, 0));
    constexpr float kMargin = 1.5f;
    const float inner = static_cast<float>(icon_px) - 2.f * kMargin;
    const float scale = inner / 24.f;
    g.TranslateTransform(kMargin, kMargin);
    g.ScaleTransform(scale, scale);
    gdip::Pen pen(gdip_rgb(ink), 2.f);
    pen.SetStartCap(gdip::LineCapRound);
    pen.SetEndCap(gdip::LineCapRound);
    pen.SetLineJoin(gdip::LineJoinRound);

    switch (which) {
      case MenuBarIcon::Exit:
        // lucide log-out — door + arrow
        g.DrawRectangle(&pen, 9.f, 3.f, 11.f, 18.f);
        g.DrawLine(&pen, 3.f, 12.f, 14.f, 12.f);
        {
          gdip::PointF tip[] = {{11.f, 8.f}, {15.f, 12.f}, {11.f, 16.f}};
          g.DrawLines(&pen, tip, 3);
        }
        break;
      case MenuBarIcon::Refresh:
        g.DrawArc(&pen, 4.f, 4.f, 16.f, 16.f, -50.f, 200.f);
        {
          gdip::PointF tip[] = {{16.5f, 4.f}, {20.f, 4.f}, {20.f, 7.5f}};
          g.DrawLines(&pen, tip, 3);
        }
        g.DrawArc(&pen, 4.f, 4.f, 16.f, 16.f, 130.f, 200.f);
        {
          gdip::PointF tip[] = {{7.5f, 20.f}, {4.f, 20.f}, {4.f, 16.5f}};
          g.DrawLines(&pen, tip, 3);
        }
        break;
      case MenuBarIcon::Start:
        g.DrawRectangle(&pen, 2.f, 3.f, 20.f, 14.f);
        g.DrawLine(&pen, 12.f, 17.f, 12.f, 21.f);
        g.DrawLine(&pen, 8.f, 21.f, 16.f, 21.f);
        break;
      case MenuBarIcon::Close:
        g.DrawLine(&pen, 18.f, 6.f, 6.f, 18.f);
        g.DrawLine(&pen, 6.f, 6.f, 18.f, 18.f);
        break;
      case MenuBarIcon::CloseAll:
        g.DrawEllipse(&pen, 3.f, 3.f, 18.f, 18.f);
        g.DrawLine(&pen, 15.f, 9.f, 9.f, 15.f);
        g.DrawLine(&pen, 9.f, 9.f, 15.f, 15.f);
        break;
      case MenuBarIcon::About:
        // lucide info
        g.DrawEllipse(&pen, 3.f, 3.f, 18.f, 18.f);
        g.DrawLine(&pen, 12.f, 11.f, 12.f, 16.f);
        g.DrawEllipse(&pen, 11.f, 7.f, 2.f, 2.f);
        break;
      default:
        break;
    }
    bmp.GetHBITMAP(gdip::Color(0, 0, 0, 0), &hbmp);
  }
  gdip::GdiplusShutdown(token);
  return hbmp;
}

void attach_console_menu_icons(ConsoleState* st) {
  if (!st || !st->hwnd) {
    return;
  }
  HMENU menu = GetMenu(st->hwnd);
  if (!menu) {
    return;
  }
  const int icon_px = dip(14, st->dpi);
  for (int i = 0; i < static_cast<int>(MenuBarIcon::Count); ++i) {
    if (st->menu_icons[i]) {
      DeleteObject(st->menu_icons[i]);
      st->menu_icons[i] = nullptr;
    }
    st->menu_icons[i] =
        make_menu_bar_bitmap(static_cast<MenuBarIcon>(i), icon_px, kTextSecondary);
  }

  struct Item {
    UINT cmd;
    MenuBarIcon icon;
  };
  const Item items[] = {
      {kCmdExit, MenuBarIcon::Exit},
      {kCmdRefresh, MenuBarIcon::Refresh},
      {kCmdStartSession, MenuBarIcon::Start},
      {kCmdCloseSession, MenuBarIcon::Close},
      {kCmdCloseAll, MenuBarIcon::CloseAll},
      {kCmdAbout, MenuBarIcon::About},
  };
  for (const Item& it : items) {
    MENUITEMINFOW mii{};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_BITMAP;
    mii.hbmpItem = st->menu_icons[static_cast<int>(it.icon)];
    SetMenuItemInfoW(menu, it.cmd, FALSE, &mii);
  }

  MENUINFO mi{};
  mi.cbSize = sizeof(mi);
  mi.fMask = MIM_BACKGROUND | MIM_STYLE;
  mi.dwStyle = MNS_CHECKORBMP;
  mi.hbrBack = st->menu_chrome_br;
  SetMenuInfo(menu, &mi);
  DrawMenuBar(st->hwnd);
}

HACCEL create_console_accel() {
  // Local chrome shortcuts (directory / shell). Session grab is out of scope.
  ACCEL accels[] = {
      {FVIRTKEY | FNOINVERT, VK_F5, static_cast<WORD>(kCmdRefresh)},
      {FVIRTKEY | FCONTROL | FNOINVERT, VK_RETURN, static_cast<WORD>(kCmdStartSession)},
      {FVIRTKEY | FCONTROL | FNOINVERT, 'W', static_cast<WORD>(kCmdCloseSession)},
      {FVIRTKEY | FCONTROL | FSHIFT | FNOINVERT, 'W', static_cast<WORD>(kCmdCloseAll)},
      {FVIRTKEY | FNOINVERT, VK_F1, static_cast<WORD>(kCmdAbout)},
      {FVIRTKEY | FCONTROL | FNOINVERT, VK_TAB, static_cast<WORD>(kCmdNextTab)},
      {FVIRTKEY | FCONTROL | FSHIFT | FNOINVERT, VK_TAB, static_cast<WORD>(kCmdPrevTab)},
      {FVIRTKEY | FNOINVERT, VK_F6, static_cast<WORD>(kCmdCyclePane)},
  };
  return CreateAcceleratorTableW(accels, static_cast<int>(ARRAYSIZE(accels)));
}

void show_about_dialog(HWND owner) {
  wchar_t buf[320] = {};
  _snwprintf_s(buf, _TRUNCATE,
               L"Road Desk Viewer\n"
               L"版本 %hs\n"
               L"操作端 — 连接目录并远控被控端桌面\n\n"
               L"编译 %hs %hs",
               ROAD_DESK_VERSION_STRING, ROAD_DESK_BUILD_DATE, ROAD_DESK_BUILD_TIME);
  MessageBoxW(owner, buf, L"Road Desk", MB_OK | MB_ICONINFORMATION);
}

bool confirm_exit_if_sessions(ConsoleState* st) {
  if (!st || st->sessions.empty()) {
    return true;
  }
  const int r = MessageBoxW(st->hwnd,
                            L"有活动远控会话，退出将全部断开。确定退出？", L"Road Desk",
                            MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
  return r == IDOK;
}

void cmd_start_session(ConsoleState* st) {
  if (!st) {
    return;
  }
  if (session_toolbar_starts(st)) {
    open_session_for_device(st, selected_list_device_id(st));
    return;
  }
  const int device_id = selected_list_device_id(st);
  if (device_id >= 0) {
    open_session_for_device(st, device_id);  // focus existing
  } else {
    set_status(st, L"请先在列表中选择被控端");
  }
}

void cmd_close_active_session(ConsoleState* st) {
  if (!st) {
    return;
  }
  if (st->active_tab >= 0 && st->active_tab < static_cast<int>(st->sessions.size())) {
    close_session_at(st, st->active_tab);
  }
}

void cmd_session_toolbar(ConsoleState* st) {
  if (!st) {
    return;
  }
  if (session_toolbar_starts(st)) {
    open_session_for_device(st, selected_list_device_id(st));
  } else if (st->active_tab >= 0 &&
             st->active_tab < static_cast<int>(st->sessions.size())) {
    close_session_at(st, st->active_tab);
  } else {
    cmd_start_session(st);
  }
}

LRESULT CALLBACK ConsoleProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  ConsoleState* st = g_console;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    st = static_cast<ConsoleState*>(cs->lpCreateParams);
    g_console = st;
    if (st) {
      st->hwnd = hwnd;
    }
  }
  if (!st) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  switch (msg) {
    case WM_CREATE: {
      st->dpi = system_dpi();
      st->ui_font = create_ui_font(st->dpi, rd::kFontBodyPt);
      st->caption_font = create_ui_font(st->dpi, rd::kFontCaptionPt);
      st->toolbar_h = dip(kToolbarHDip, st->dpi);
      st->status_h = dip(kStatusHDip, st->dpi);
      st->splitter_w = dip(kSplitterWDip, st->dpi);
      st->close_btn_w = dip(kCloseBtnWDip, st->dpi);
      st->tab_nav_w = dip(kTabNavWDip, st->dpi);
      st->float_badge_w = dip(kFloatBadgeWDip, st->dpi);
      st->tab_h = measure_tab_height(st->ui_font);
      SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(st->ui_font), TRUE);

      st->menu_chrome_br = CreateSolidBrush(kChrome);
      attach_console_menu_icons(st);

      const DWORD tb_style = WS_CHILD | WS_VISIBLE | CCS_NODIVIDER | CCS_NOPARENTALIGN |
                             CCS_NORESIZE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | TBSTYLE_TRANSPARENT;
      st->toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr, tb_style, 0, 0, 100,
                                    st->toolbar_h, hwnd, reinterpret_cast<HMENU>(IDC_TOOLBAR),
                                    st->instance, nullptr);
      SendMessageW(st->toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
      SendMessageW(st->toolbar, TB_SETINDENT, dip(kToolbarPadXDip, st->dpi), 0);
      {
        TBMETRICS tm{};
        tm.cbSize = sizeof(tm);
        tm.dwMask = TBMF_PAD | TBMF_BUTTONSPACING;
        tm.cxPad = dip(4, st->dpi);
        tm.cyPad = 0;
        tm.cxButtonSpacing = dip(4, st->dpi);
        tm.cyButtonSpacing = 0;
        SendMessageW(st->toolbar, TB_SETMETRICS, 0, reinterpret_cast<LPARAM>(&tm));
      }
      // Fixed square buttons = toolbar height so glyphs stay vertically centered.
      const int btn = st->toolbar_h;
      SendMessageW(st->toolbar, TB_SETBUTTONSIZE, 0, MAKELONG(btn, btn));
      const int icon_px = dip(kToolbarIconDip, st->dpi);
      st->toolbar_il = build_toolbar_images(icon_px, kTextSecondary);
      st->toolbar_il_on_accent = build_toolbar_images(icon_px, rd::kColorTextOnBrand);
      st->toolbar_il_muted = build_toolbar_images(icon_px, kTextMuted);
      SendMessageW(st->toolbar, TB_SETIMAGELIST, 0,
                   reinterpret_cast<LPARAM>(st->toolbar_il));
      SendMessageW(st->toolbar, TB_SETDISABLEDIMAGELIST, 0,
                   reinterpret_cast<LPARAM>(st->toolbar_il_muted));
      {
        // No BTNS_AUTOSIZE — keeps 32×32 hit targets aligned to design.
        const BYTE style = BTNS_BUTTON;
        const BYTE check = BTNS_CHECK;
        const int sep_w = dip(8, st->dpi);
        TBBUTTON btns[] = {
            {kTbImgRefresh, kCmdRefresh, TBSTATE_ENABLED, style, {}, 0, 0},
            {sep_w, 0, TBSTATE_ENABLED, BTNS_SEP, {}, 0, 0},
            {kTbImgStart, kCmdSession, 0, style, {}, 0, 0},
            {kTbImgCloseAll, kCmdCloseAll, 0, style, {}, 0, 0},
            {sep_w, 0, TBSTATE_ENABLED, BTNS_SEP, {}, 0, 0},
            {kTbImgScreenshot, kCmdScreenshot, 0, style, {}, 0, 0},
            {kTbImgFullscreen, kCmdFullscreen, 0, style, {}, 0, 0},
            {kTbImgViewOnly, kCmdViewOnly, 0, check, {}, 0, 0},
        };
        SendMessageW(st->toolbar, TB_ADDBUTTONSW, ARRAYSIZE(btns),
                     reinterpret_cast<LPARAM>(btns));
      }
      SendMessageW(st->toolbar, TB_SETBUTTONSIZE, 0, MAKELONG(btn, btn));
      // Intentionally no TB_AUTOSIZE — layout() sizes the strip to full client width.
      st->tree = CreateWindowExW(0, WC_TREEVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASLINES |
                                     TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS |
                                     TVS_FULLROWSELECT,
                                 0, 0, 100, 100, hwnd, reinterpret_cast<HMENU>(IDC_TREE),
                                 st->instance, nullptr);
      TreeView_SetBkColor(st->tree, kPanel);
      TreeView_SetTextColor(st->tree, kTextPrimary);
      TreeView_SetLineColor(st->tree, kBorderStrong);

      st->tab_strip =
          CreateWindowExW(0, kTabStripClass, L"",
                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 100,
                          st->tab_h, hwnd, reinterpret_cast<HMENU>(IDC_TABSTRIP), st->instance,
                          nullptr);

      st->list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                                     LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                 0, 0, 100, 100, hwnd, reinterpret_cast<HMENU>(IDC_LIST),
                                 st->instance, nullptr);
      ListView_SetExtendedListViewStyle(st->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
      ListView_SetBkColor(st->list, kPanel);
      ListView_SetTextColor(st->list, kTextPrimary);
      ListView_SetTextBkColor(st->list, kPanel);
      LVCOLUMNW col{};
      col.mask = LVCF_TEXT | LVCF_WIDTH;
      col.pszText = const_cast<wchar_t*>(L"名称");
      col.cx = 160;
      ListView_InsertColumn(st->list, 0, &col);
      col.pszText = const_cast<wchar_t*>(L"IP地址");
      col.cx = 130;
      ListView_InsertColumn(st->list, 1, &col);
      col.pszText = const_cast<wchar_t*>(L"版本");
      col.cx = 70;
      ListView_InsertColumn(st->list, 2, &col);
      col.pszText = const_cast<wchar_t*>(L"角色");
      col.cx = 70;
      ListView_InsertColumn(st->list, 3, &col);
      col.pszText = const_cast<wchar_t*>(L"备注");
      col.cx = 140;
      ListView_InsertColumn(st->list, 4, &col);

      st->session_area =
          CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0,
                          100, 100, hwnd, reinterpret_cast<HMENU>(IDC_SESSION_HOST),
                          st->instance, nullptr);

      st->status = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                   hwnd, reinterpret_cast<HMENU>(IDC_STATUS), st->instance,
                                   nullptr);
      SendMessageW(st->status, SB_SETBKCOLOR, 0, static_cast<LPARAM>(kChrome));

      st->splitter_chip =
          CreateWindowExW(0, kSplitterChipClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, hwnd,
                          nullptr, st->instance, nullptr);
      if (st->splitter_chip) {
        ShowWindow(st->splitter_chip, SW_HIDE);
      }

      apply_ui_font(st, st->toolbar);
      apply_ui_font(st, st->tree);
      apply_ui_font(st, st->list);
      apply_ui_font(st, st->tab_strip);
      apply_caption_font(st, st->status);
      layout_status_parts(st, 1100);
      set_status(st, catalog_ready_status());

      fill_tree(st);
      layout(st);
      if (st->tree) {
        SetFocus(st->tree);
      }
      return 0;
    }
    case WM_SIZE:
      layout(st);
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      const int top = st->body_top > 0 ? st->body_top : st->toolbar_h;
      const int bottom = rc.bottom - st->status_h;
      if (bottom > top) {
        RECT split{st->tree_width, top, st->tree_width + st->splitter_w, bottom};
        const COLORREF fill =
            (st->dragging_splitter || st->splitter_hot) ? kAccent : kSplitter;
        HBRUSH br = CreateSolidBrush(fill);
        FillRect(hdc, &split, br);
        DeleteObject(br);
        if (st->dragging_splitter || st->splitter_hot) {
          const int mid_x = st->tree_width + st->splitter_w / 2;
          const int mid_y = (top + bottom) / 2;
          HBRUSH dot = CreateSolidBrush(rd::kColorTextOnBrand);
          for (int i = -1; i <= 1; ++i) {
            RECT dr{mid_x - 1, mid_y + i * 5 - 1, mid_x + 1, mid_y + i * 5 + 1};
            FillRect(hdc, &dr, dot);
          }
          DeleteObject(dot);
        }
      }
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_SETCURSOR: {
      if (LOWORD(lparam) == HTCLIENT) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int top = st->body_top > 0 ? st->body_top : st->toolbar_h;
        if (pt.y > top && pt.y < rc.bottom - st->status_h && pt.x >= st->tree_width &&
            pt.x < st->tree_width + st->splitter_w) {
          SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
          return TRUE;
        }
      }
      break;
    }
    case WM_SESSION_META: {
      // Agent version, reconnect status, and/or chrome changes (fullscreen dock/undock).
      sync_session_floating_flags(st);
      layout(st);
      if (st->tab_strip) {
        InvalidateRect(st->tab_strip, nullptr, TRUE);
      }
      if (st->active_tab >= 0 && st->active_tab < static_cast<int>(st->sessions.size())) {
        SessionHost* host = st->sessions[static_cast<size_t>(st->active_tab)].host.get();
        if (host) {
          const std::wstring status = host->status_text();
          if (!status.empty()) {
            set_status(st, status.c_str());
          } else if (host->connected()) {
            if (host->host_forced_view_only()) {
              set_status(st, L"仅观看（被控端已有控制端）");
            } else if (host->view_only()) {
              set_status(st, L"仅查看模式：键鼠输入已冻结");
            } else {
              set_status(st, L"已连接（控制）");
            }
          }
        }
      }
      return 0;
    }
    case WM_SESSION_CLOSED: {
      auto* host = reinterpret_cast<SessionHost*>(lparam);
      for (int i = 0; i < static_cast<int>(st->sessions.size()); ++i) {
        if (st->sessions[static_cast<size_t>(i)].host.get() == host) {
          st->sessions[static_cast<size_t>(i)].host.reset();
          remove_session_at(st, i);
          break;
        }
      }
      return 0;
    }
    case WM_CLOSE:
      if (!confirm_exit_if_sessions(st)) {
        return 0;
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_COMMAND: {
      const int cmd = LOWORD(wparam);
      if (cmd == kCmdRefresh) {
        refresh_directory(st);
      } else if (cmd == kCmdExit) {
        SendMessageW(hwnd, WM_CLOSE, 0, 0);
      } else if (cmd == kCmdAbout) {
        show_about_dialog(hwnd);
      } else if (cmd == kCmdStartSession) {
        cmd_start_session(st);
      } else if (cmd == kCmdCloseSession) {
        cmd_close_active_session(st);
      } else if (cmd == kCmdNextTab) {
        cmd_next_tab(st);
      } else if (cmd == kCmdPrevTab) {
        cmd_prev_tab(st);
      } else if (cmd == kCmdCyclePane) {
        cmd_cycle_pane(st);
      } else if (cmd == kCmdSession) {
        cmd_session_toolbar(st);
      } else if (cmd == kCmdCloseAll) {
        close_all_sessions(st);
      } else if (cmd == kCmdScreenshot || cmd == kCmdFullscreen || cmd == kCmdViewOnly) {
        if (st->active_tab >= 0 &&
            st->active_tab < static_cast<int>(st->sessions.size())) {
          SessionHost* host =
              st->sessions[static_cast<size_t>(st->active_tab)].host.get();
          if (cmd == kCmdScreenshot) {
            if (!host || !host->connected()) {
              set_status(st, L"截图失败：会话未连接");
            } else {
              const std::wstring path = default_screenshot_path(
                  st->sessions[static_cast<size_t>(st->active_tab)].title);
              if (host->save_screenshot(path)) {
                set_status(st, (L"截图已保存：" + path).c_str());
                const std::wstring args = L"/select,\"" + path + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                              SW_SHOWNORMAL);
              } else {
                set_status(st, L"截图失败：暂无画面或无法写入文件");
              }
            }
          } else if (cmd == kCmdFullscreen) {
            if (host) {
              host->toggle_fullscreen();
              sync_session_floating_flags(st);
              layout(st);
            }
          } else if (cmd == kCmdViewOnly && host) {
            if (host->host_forced_view_only()) {
              set_status(st, L"被控端已有控制端，本会话仅观看");
            } else {
              host->set_view_only(!host->view_only());
              sync_chrome_commands(st);
              set_status(st, host->view_only() ? L"仅查看模式：键鼠输入已冻结"
                                               : L"已恢复键鼠控制");
            }
          }
        }
      }
      return 0;
    }
    case WM_NOTIFY: {
      const auto* hdr = reinterpret_cast<NMHDR*>(lparam);
      if (hdr->hwndFrom == st->toolbar && hdr->code == NM_CUSTOMDRAW) {
        auto* cd = reinterpret_cast<LPNMTBCUSTOMDRAW>(lparam);
        switch (cd->nmcd.dwDrawStage) {
          case CDDS_PREPAINT: {
            RECT rc{};
            GetClientRect(st->toolbar, &rc);
            HBRUSH br = CreateSolidBrush(kChrome);
            FillRect(cd->nmcd.hdc, &rc, br);
            DeleteObject(br);
            HPEN pen = CreatePen(PS_SOLID, 1, kBorderSubtle);
            HGDIOBJ old = SelectObject(cd->nmcd.hdc, pen);
            MoveToEx(cd->nmcd.hdc, rc.left, rc.bottom - 1, nullptr);
            LineTo(cd->nmcd.hdc, rc.right, rc.bottom - 1);
            SelectObject(cd->nmcd.hdc, old);
            DeleteObject(pen);
            return CDRF_NOTIFYITEMDRAW;
          }
          case CDDS_ITEMPREPAINT:
            return paint_toolbar_button(st, cd);
          default:
            break;
        }
        return CDRF_DODEFAULT;
      }
      if (hdr->hwndFrom == st->toolbar && hdr->code == TBN_HOTITEMCHANGE) {
        // Force both old/new buttons to repaint so hover chrome updates immediately.
        const auto* hot = reinterpret_cast<LPNMTBHOTITEM>(lparam);
        if (hot->idOld != 0) {
          RECT rc{};
          if (SendMessageW(st->toolbar, TB_GETRECT, hot->idOld, reinterpret_cast<LPARAM>(&rc))) {
            InvalidateRect(st->toolbar, &rc, FALSE);
          }
        }
        if (hot->idNew != 0) {
          RECT rc{};
          if (SendMessageW(st->toolbar, TB_GETRECT, hot->idNew, reinterpret_cast<LPARAM>(&rc))) {
            InvalidateRect(st->toolbar, &rc, FALSE);
          }
        }
        st->toolbar_hot_cmd = static_cast<int>(hot->idNew);
        return 0;
      }
      if (hdr->hwndFrom == st->tree && hdr->code == TVN_SELCHANGEDW) {
        const auto* ntv = reinterpret_cast<NMTREEVIEWW*>(lparam);
        st->selected_group_id = static_cast<int>(ntv->itemNew.lParam);
        fill_list(st);
      }
      if (hdr->hwndFrom == st->tree && hdr->code == NM_DBLCLK) {
        HTREEITEM h = TreeView_GetSelection(st->tree);
        if (h) {
          TVITEMW it{};
          it.mask = TVIF_PARAM;
          it.hItem = h;
          if (TreeView_GetItem(st->tree, &it)) {
            open_session_for_device(st, static_cast<int>(it.lParam));
          }
        }
      }
      if (hdr->hwndFrom == st->tree && hdr->code == NM_RETURN) {
        HTREEITEM h = TreeView_GetSelection(st->tree);
        if (h) {
          TVITEMW it{};
          it.mask = TVIF_PARAM;
          it.hItem = h;
          if (TreeView_GetItem(st->tree, &it)) {
            open_session_for_device(st, static_cast<int>(it.lParam));
          }
        }
        return TRUE;
      }
      if (hdr->hwndFrom == st->list && hdr->code == NM_SETFOCUS) {
        ensure_list_selection(st);
        return 0;
      }
      if (hdr->hwndFrom == st->list && hdr->code == LVN_ITEMCHANGED) {
        const auto* lv = reinterpret_cast<NMLISTVIEW*>(lparam);
        if ((lv->uChanged & LVIF_STATE) &&
            ((lv->uOldState ^ lv->uNewState) & LVIS_SELECTED)) {
          sync_chrome_commands(st);
        }
      }
      if (hdr->hwndFrom == st->list && hdr->code == NM_DBLCLK) {
        int i = ListView_GetNextItem(st->list, -1, LVNI_SELECTED);
        if (i >= 0) {
          LVITEMW it{};
          it.mask = LVIF_PARAM;
          it.iItem = i;
          ListView_GetItem(st->list, &it);
          open_session_for_device(st, static_cast<int>(it.lParam));
        }
      }
      if (hdr->hwndFrom == st->list && hdr->code == NM_RETURN) {
        int i = ListView_GetNextItem(st->list, -1, LVNI_SELECTED);
        if (i >= 0) {
          LVITEMW it{};
          it.mask = LVIF_PARAM;
          it.iItem = i;
          ListView_GetItem(st->list, &it);
          open_session_for_device(st, static_cast<int>(it.lParam));
        }
        return TRUE;
      }
      if (hdr->hwndFrom == st->toolbar && hdr->code == TBN_GETINFOTIP) {
        auto* info = reinterpret_cast<NMTBGETINFOTIPW*>(lparam);
        const wchar_t* tip = nullptr;
        switch (info->iItem) {
          case kCmdRefresh:
            tip = L"刷新目录";
            break;
          case kCmdSession:
            tip = session_toolbar_starts(st) ? L"启动会话" : L"关闭会话";
            break;
          case kCmdCloseAll:
            tip = L"全部断开";
            break;
          case kCmdScreenshot:
            tip = L"截图（保存到桌面）";
            break;
          case kCmdFullscreen:
            tip = L"全屏（Esc 退出）";
            break;
          case kCmdViewOnly: {
            const bool forced =
                st->active_tab >= 0 &&
                st->active_tab < static_cast<int>(st->sessions.size()) &&
                st->sessions[static_cast<size_t>(st->active_tab)].host &&
                st->sessions[static_cast<size_t>(st->active_tab)].host->host_forced_view_only();
            tip = forced ? L"仅观看（被控端已有控制端）" : L"仅查看（冻结键鼠输入）";
            break;
          }
        }
        if (tip) {
          wcsncpy_s(info->pszText, info->cchTextMax, tip, _TRUNCATE);
        }
        return 0;
      }
      return 0;
    }
    case WM_LBUTTONDOWN: {
      RECT rc{};
      GetClientRect(hwnd, &rc);
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      const int top = st->body_top > 0 ? st->body_top : st->toolbar_h;
      const int split = st->tree_width;
      if (y > top && y < rc.bottom - st->status_h && x >= split && x < split + st->splitter_w) {
        st->dragging_splitter = true;
        st->splitter_hot = true;
        SetCapture(hwnd);
        RECT split_rc{split, top, split + st->splitter_w, rc.bottom - st->status_h};
        InvalidateRect(hwnd, &split_rc, FALSE);
        update_splitter_chip(st);
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      RECT rc{};
      GetClientRect(hwnd, &rc);
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      const int top = st->body_top > 0 ? st->body_top : st->toolbar_h;
      const bool over = y > top && y < rc.bottom - st->status_h && x >= st->tree_width &&
                        x < st->tree_width + st->splitter_w;
      if (st->dragging_splitter) {
        st->tree_width = x;
        layout(st);
        SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
        update_splitter_chip(st);
      } else if (over != st->splitter_hot) {
        st->splitter_hot = over;
        RECT split_rc{st->tree_width, top, st->tree_width + st->splitter_w, rc.bottom - st->status_h};
        InvalidateRect(hwnd, &split_rc, FALSE);
      }
      return 0;
    }
    case WM_LBUTTONUP:
      if (st->dragging_splitter) {
        st->dragging_splitter = false;
        ReleaseCapture();
        hide_splitter_chip(st);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int top = st->body_top > 0 ? st->body_top : st->toolbar_h;
        RECT split_rc{st->tree_width, top, st->tree_width + st->splitter_w, rc.bottom - st->status_h};
        InvalidateRect(hwnd, &split_rc, FALSE);
      }
      return 0;
    case WM_CAPTURECHANGED:
      if (st->dragging_splitter && reinterpret_cast<HWND>(lparam) != hwnd) {
        st->dragging_splitter = false;
        hide_splitter_chip(st);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_DESTROY:
      destroy_drag_ghost(st);
      hide_splitter_chip(st);
      if (st->splitter_chip) {
        DestroyWindow(st->splitter_chip);
        st->splitter_chip = nullptr;
      }
      for (auto& tab : st->sessions) {
        if (tab.host) {
          tab.host->clear_closed_handler();
          tab.host->close();
        }
      }
      st->sessions.clear();
      if (st->ui_font) {
        DeleteObject(st->ui_font);
        st->ui_font = nullptr;
      }
      if (st->caption_font) {
        DeleteObject(st->caption_font);
        st->caption_font = nullptr;
      }
      if (st->menu_chrome_br) {
        DeleteObject(st->menu_chrome_br);
        st->menu_chrome_br = nullptr;
      }
      for (HBITMAP& bmp : st->menu_icons) {
        if (bmp) {
          DeleteObject(bmp);
          bmp = nullptr;
        }
      }
      if (st->toolbar_il) {
        ImageList_Destroy(st->toolbar_il);
        st->toolbar_il = nullptr;
      }
      if (st->toolbar_il_on_accent) {
        ImageList_Destroy(st->toolbar_il_on_accent);
        st->toolbar_il_on_accent = nullptr;
      }
      if (st->toolbar_il_muted) {
        ImageList_Destroy(st->toolbar_il_muted);
        st->toolbar_il_muted = nullptr;
      }
      g_console = nullptr;
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool register_console_classes(HINSTANCE instance) {
  static HBRUSH chrome_bg = CreateSolidBrush(kChrome);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = ConsoleProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = chrome_bg;
  wc.lpszClassName = kConsoleClass;
  rd::rd_apply_wndclass_icons(&wc, instance);
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  WNDCLASSEXW tc{};
  tc.cbSize = sizeof(tc);
  tc.lpfnWndProc = TabStripProc;
  tc.hInstance = instance;
  tc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  tc.hbrBackground = chrome_bg;
  tc.lpszClassName = kTabStripClass;
  if (!RegisterClassExW(&tc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  WNDCLASSEXW gc{};
  gc.cbSize = sizeof(gc);
  gc.lpfnWndProc = DragGhostProc;
  gc.hInstance = instance;
  gc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  gc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  gc.lpszClassName = kDragGhostClass;
  if (!RegisterClassExW(&gc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  WNDCLASSEXW chip{};
  chip.cbSize = sizeof(chip);
  chip.lpfnWndProc = SplitterChipProc;
  chip.hInstance = instance;
  chip.hCursor = LoadCursor(nullptr, IDC_ARROW);
  chip.hbrBackground = nullptr;
  chip.lpszClassName = kSplitterChipClass;
  if (!RegisterClassExW(&chip) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  return SessionHost::register_class(instance);
}

}  // namespace

int run_console(HINSTANCE instance, int /*show_cmd*/, const ConnectDefaults& connect) {
  INITCOMMONCONTROLSEX icc{sizeof(icc),
                           ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
  InitCommonControlsEx(&icc);

  if (!register_console_classes(instance)) {
    return 1;
  }

  ConsoleState state;
  state.instance = instance;
  state.connect = connect;
  g_console = &state;

  HMENU menu = build_console_menu();
  constexpr int kConsoleW = 1100;
  constexpr int kConsoleH = 720;
  HWND hwnd = CreateWindowExW(0, kConsoleClass, L"Road Desk Viewer", WS_OVERLAPPEDWINDOW, 0, 0,
                              kConsoleW, kConsoleH, nullptr, menu, instance, &state);
  if (!hwnd) {
    return 1;
  }
  rd::rd_set_window_icons(hwnd, instance);
  state.accel = create_console_accel();
  // Center restore bounds on the work area. Do not pass WinMain's show_cmd
  // (often SW_SHOWDEFAULT): Explorer STARTUPINFO would otherwise place the
  // window at the cascade top-left. Always open maximized for the console.
  RECT rc{};
  GetWindowRect(hwnd, &rc);
  RECT wa{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  const int win_w = rc.right - rc.left;
  const int win_h = rc.bottom - rc.top;
  SetWindowPos(hwnd, nullptr, wa.left + (wa.right - wa.left - win_w) / 2,
               wa.top + (wa.bottom - wa.top - win_h) / 2, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  ShowWindow(hwnd, SW_SHOWMAXIMIZED);
  UpdateWindow(hwnd);
  SessionHost::install_keyboard_hook(instance);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (state.accel && TranslateAcceleratorW(hwnd, state.accel, &msg)) {
      continue;
    }
    if (IsDialogMessageW(hwnd, &msg)) {
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  SessionHost::uninstall_keyboard_hook();
  if (state.accel) {
    DestroyAcceleratorTable(state.accel);
    state.accel = nullptr;
  }
  g_console = nullptr;
  return 0;
}

int run_direct_session(HINSTANCE instance, int show_cmd, const ConnectDefaults& connect) {
  if (!SessionHost::register_class(instance)) {
    return 1;
  }
  SessionHost host;
  bool closed = false;
  if (!host.open(instance, nullptr, L"Road Desk Viewer", connect,
                 [&](SessionHost*) { closed = true; })) {
    return 1;
  }
  ShowWindow(host.hwnd(), show_cmd);
  SessionHost::set_keyboard_target(&host);
  SessionHost::install_keyboard_hook(instance);

  MSG msg{};
  while (!closed && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  SessionHost::uninstall_keyboard_hook();
  return 0;
}

}  // namespace road_desk::viewer
