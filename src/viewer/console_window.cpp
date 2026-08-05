#include "console_window.h"

#include "address_book.h"
#include "product_version.h"
#include "session_host.h"

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

constexpr wchar_t kConsoleClass[] = L"RoadDeskConsoleWindow";
constexpr wchar_t kTabStripClass[] = L"RoadDeskTabStrip";
constexpr wchar_t kDragGhostClass[] = L"RoadDeskDragGhost";
constexpr int kToolbarHDip = 32;  // rd: 工具栏条
constexpr int kToolbarIconDip = 16;
constexpr int kToolbarPadXDip = 10;  // 工具栏左右内边距
constexpr int kStatusH = 24;
constexpr int kTabHDefault = 30;
constexpr int kCloseBtnW = 18;
constexpr int kSplitterW = 4;
constexpr int kMaxSessions = 8;
constexpr int kTabCatalog = -1;
constexpr int kTabHitClose = -3;  // session close button (drag_tab_index holds session idx)
constexpr int kGhostW = 260;
constexpr int kGhostH = 160;
constexpr int kGhostTitleH = 28;

// Design tokens (docs/ui-design-system.md).
constexpr COLORREF kChrome = RGB(236, 240, 245);       // rd.color.surface.chrome
constexpr COLORREF kPanel = RGB(255, 255, 255);         // rd.color.surface.panel
constexpr COLORREF kBorderSubtle = RGB(220, 226, 234);  // rd.color.border.subtle
constexpr COLORREF kBorderStrong = RGB(180, 190, 204);  // rd.color.border.strong
constexpr COLORREF kTextPrimary = RGB(28, 34, 46);      // rd.color.text.primary
constexpr COLORREF kTextSecondary = RGB(88, 98, 114);   // rd.color.text.secondary
constexpr COLORREF kTextMuted = RGB(140, 150, 164);     // rd.color.text.muted
constexpr COLORREF kIconInk = RGB(88, 98, 114);         // toolbar glyphs
constexpr COLORREF kRowHover = RGB(232, 238, 246);      // rd.color.row.hover
constexpr COLORREF kRowSelected = RGB(210, 226, 244);   // rd.color.row.selected
constexpr COLORREF kAccent = RGB(47, 107, 168);         // rd.color.accent
constexpr COLORREF kAccentHover = RGB(38, 90, 145);     // rd.color.accent.hover
constexpr COLORREF kAccentPressed = RGB(30, 74, 122);   // rd.color.accent.pressed

enum : int {
  IDC_TREE = 1001,
  IDC_LIST = 1002,
  IDC_TABSTRIP = 1003,
  IDC_STATUS = 1004,
  IDC_TOOLBAR = 1005,
  IDC_SESSION_HOST = 1006,
};

// Toolbar command ids.
constexpr int kCmdRefresh = 10;
constexpr int kCmdCloseSession = 11;
constexpr int kCmdCloseAll = 12;
constexpr int kCmdScreenshot = 13;
constexpr int kCmdFullscreen = 14;
constexpr int kCmdViewOnly = 15;

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
  HWND tree = nullptr;
  HWND list = nullptr;
  HWND tab_strip = nullptr;
  HWND status = nullptr;
  HWND session_area = nullptr;
  HFONT ui_font = nullptr;
  ConnectDefaults connect;
  int dpi = 96;
  int toolbar_h = kToolbarHDip;
  int tree_width = 240;
  int tab_h = kTabHDefault;
  int selected_group_id = 0;
  int active_tab = kTabCatalog;  // kTabCatalog or session index
  int toolbar_hot_cmd = -1;      // for hover transition invalidate
  std::vector<SessionTab> sessions;
  bool dragging_splitter = false;
  bool dragging_tab = false;
  bool drag_tear_armed = false;  // moved far enough; detach only on LBUTTONUP
  int drag_tab_index = -1;
  int close_hit_index = -1;  // session index when close button pressed
  POINT drag_start{};
  HWND drag_ghost = nullptr;
  std::wstring drag_ghost_title;
};

int system_dpi() {
  HDC screen = GetDC(nullptr);
  const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
  if (screen) {
    ReleaseDC(nullptr, screen);
  }
  return dpi > 0 ? dpi : 96;
}

int dip(int v, int dpi) {
  return MulDiv(v, dpi > 0 ? dpi : 96, 96);
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

// Draw chrome face; default handler still paints the glyph.
LRESULT paint_toolbar_button(ConsoleState* st, LPNMTBCUSTOMDRAW cd) {
  if (!st || !cd) {
    return CDRF_DODEFAULT;
  }
  // Separators use idCommand 0; real cmds start at kCmdRefresh.
  if (cd->nmcd.dwItemSpec == 0) {
    paint_toolbar_separator(cd->nmcd.hdc, cd->nmcd.rc, st->dpi);
    return CDRF_SKIPDEFAULT;
  }

  const UINT state = cd->nmcd.uItemState;
  const bool disabled = (state & CDIS_DISABLED) != 0;
  const bool pressed = (state & CDIS_SELECTED) != 0;
  const bool hot = (state & CDIS_HOT) != 0;
  const bool checked = (state & CDIS_CHECKED) != 0;
  const bool focused = (state & CDIS_FOCUS) != 0;

  RECT face = cd->nmcd.rc;
  const int ix = dip(2, st->dpi);
  const int iy = dip(3, st->dpi);
  face.left += ix;
  face.right -= ix;
  face.top += iy;
  face.bottom -= iy;

  COLORREF fill = kPanel;
  COLORREF border = kBorderSubtle;
  if (disabled) {
    fill = kChrome;
    border = kBorderSubtle;
  } else if (pressed) {
    fill = kRowSelected;
    border = kAccentPressed;
    OffsetRect(&face, dip(1, st->dpi), dip(1, st->dpi));
  } else if (checked && hot) {
    fill = kRowSelected;
    border = kAccentHover;
  } else if (checked) {
    fill = kRowSelected;
    border = kAccent;
  } else if (hot) {
    fill = kRowHover;
    border = kBorderStrong;
  }

  const int diameter = dip(4, st->dpi) * 2;  // rd.radius.md
  fill_round_rect(cd->nmcd.hdc, face, fill, border, diameter);

  if (focused && !disabled) {
    RECT focus = face;
    InflateRect(&focus, -dip(1, st->dpi), -dip(1, st->dpi));
    fill_round_rect(cd->nmcd.hdc, focus, fill, kAccent, diameter);
  }

  // Let toolbar draw the icon; skip stock bevel / flat fill.
  return TBCDRF_NOBACKGROUND | TBCDRF_NOEDGES;
}

ConsoleState* g_console = nullptr;

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

HFONT create_ui_font() {
  NONCLIENTMETRICSW ncm{};
  ncm.cbSize = sizeof(ncm);
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
    // lfWidth!=0 forces non-square glyphs ("压扁"); keep natural aspect.
    ncm.lfMessageFont.lfWidth = 0;
    if (HFONT font = CreateFontIndirectW(&ncm.lfMessageFont)) {
      return font;
    }
  }
  LOGFONTW lf{};
  GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
  lf.lfWidth = 0;
  return CreateFontIndirectW(&lf);
}

void apply_ui_font(ConsoleState* st, HWND child) {
  if (st && st->ui_font && child) {
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(st->ui_font), TRUE);
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
  // message | viewer version (right, no label)
  constexpr int kVerPartW = 72;
  int parts[2];
  if (client_w > kVerPartW + 40) {
    parts[0] = client_w - kVerPartW;
  } else {
    parts[0] = 120;
  }
  parts[1] = -1;
  SendMessageW(st->status, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));
}

void refresh_status_fixed(ConsoleState* st) {
  if (!st || !st->status) {
    return;
  }
  wchar_t buf[64];
  _snwprintf_s(buf, _TRUNCATE, L"%hs", ROAD_DESK_VERSION_STRING);
  SendMessageW(st->status, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(buf));
}

void set_status(ConsoleState* st, const wchar_t* text) {
  if (!st || !st->status) {
    return;
  }
  refresh_status_fixed(st);
  SendMessageW(st->status, SB_SETTEXTW, 0,
               reinterpret_cast<LPARAM>(text ? text : L""));
}

void update_toolbar_state(ConsoleState* st);

void layout(ConsoleState* st) {
  if (!st || !st->hwnd) {
    return;
  }
  RECT rc{};
  GetClientRect(st->hwnd, &rc);
  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;
  const int menu_pad = 0;
  int y = menu_pad;
  if (st->toolbar) {
    MoveWindow(st->toolbar, 0, y, cw, st->toolbar_h, TRUE);
    SendMessageW(st->toolbar, TB_SETBUTTONSIZE, 0, MAKELONG(st->toolbar_h, st->toolbar_h));
    update_toolbar_state(st);
    y += st->toolbar_h;
  }
  const int body_h = ch - y - kStatusH;
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
  MoveWindow(st->tree, 0, y, tw, body_h, TRUE);

  const int work_x = tw + kSplitterW;
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
    MoveWindow(st->status, 0, ch - kStatusH, cw, kStatusH, TRUE);
    layout_status_parts(st, cw);
    refresh_status_fixed(st);
  }
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

void update_toolbar_state(ConsoleState* st) {
  if (!st || !st->toolbar) {
    return;
  }
  const bool has_sessions = !st->sessions.empty();
  const bool has_active = st->active_tab >= 0 &&
                          st->active_tab < static_cast<int>(st->sessions.size());
  SendMessageW(st->toolbar, TB_ENABLEBUTTON, kCmdCloseSession, has_sessions ? TRUE : FALSE);
  SendMessageW(st->toolbar, TB_ENABLEBUTTON, kCmdCloseAll, has_sessions ? TRUE : FALSE);
  SendMessageW(st->toolbar, TB_ENABLEBUTTON, kCmdScreenshot, has_active ? TRUE : FALSE);
  SendMessageW(st->toolbar, TB_ENABLEBUTTON, kCmdFullscreen, has_active ? TRUE : FALSE);
  SendMessageW(st->toolbar, TB_ENABLEBUTTON, kCmdViewOnly, has_active ? TRUE : FALSE);
  const bool view_only =
      has_active && st->sessions[static_cast<size_t>(st->active_tab)].host &&
      st->sessions[static_cast<size_t>(st->active_tab)].host->view_only();
  SendMessageW(st->toolbar, TB_CHECKBUTTON, kCmdViewOnly, view_only ? TRUE : FALSE);
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
  DirectoryConfig dir = load_directory_config();
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
  if (st->tab_strip) {
    InvalidateRect(st->tab_strip, nullptr, TRUE);
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
  if (!raw->open(st->instance, st->session_area, tab.title, connect,
                 [](SessionHost* h) { on_session_closed(h); })) {
    set_status(st, L"连接失败");
    return false;
  }
  st->sessions.push_back(std::move(tab));
  select_tab(st, static_cast<int>(st->sessions.size()) - 1);
  set_status(st, address_book_source() == AddressBookSource::kGateway ? L"已连接（网关目录）"
                                                                     : L"已连接（演示：统一 Host Agent）");
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
  if (tab.floating) {
    label += L" ↗";
  }
  return label;
}

int session_tab_width(HDC hdc, const SessionTab& tab) {
  const std::wstring label = session_tab_label(tab);
  SIZE sz{};
  GetTextExtentPoint32W(hdc, label.c_str(), static_cast<int>(label.size()), &sz);
  return sz.cx + 16 + kCloseBtnW;
}

// Returns kTabCatalog, session index, or -2 (miss).
// If click is on a session close button, sets *out_close_index and returns kTabHitClose.
int tab_hit_test(ConsoleState* st, int x, int* out_close_index, RECT* out_tab_rc) {
  if (out_close_index) {
    *out_close_index = -1;
  }
  HDC hdc = GetDC(st->tab_strip);
  HGDIOBJ old = st->ui_font ? SelectObject(hdc, st->ui_font) : nullptr;
  int cx = 4;
  SIZE cat_sz{};
  GetTextExtentPoint32W(hdc, L"目录", 2, &cat_sz);
  const int cat_w = cat_sz.cx + 24;
  if (x >= cx && x < cx + cat_w) {
    if (out_tab_rc) {
      *out_tab_rc = {cx, 0, cx + cat_w, st->tab_h};
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
    const int tw = session_tab_width(hdc, tab);
    if (x >= cx && x < cx + tw) {
      if (out_tab_rc) {
        *out_tab_rc = {cx, 0, cx + tw, st->tab_h};
      }
      const int close_left = cx + tw - kCloseBtnW - 2;
      if (out_close_index && x >= close_left) {
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

HIMAGELIST build_toolbar_images(int icon_px) {
  if (icon_px < 12) {
    icon_px = 12;
  }

  ULONG_PTR gdip_token = 0;
  gdip::GdiplusStartupInput gsi;
  if (gdip::GdiplusStartup(&gdip_token, &gsi, nullptr) != gdip::Ok) {
    return nullptr;
  }

  HIMAGELIST il = ImageList_Create(icon_px, icon_px, ILC_COLOR32, 6, 1);
  if (!il) {
    gdip::GdiplusShutdown(gdip_token);
    return nullptr;
  }

  enum class TbIcon : int {
    Refresh = 0,
    Close,
    PowerOff,
    Camera,
    Maximize,
    Eye,
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

    gdip::Pen pen(gdip_rgb(kIconInk), 2.f);
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
      case TbIcon::PowerOff:
        // lucide power
        g->DrawLine(&pen, 12.f, 3.f, 12.f, 12.f);
        {
          gdip::GraphicsPath arc;
          arc.AddArc(4.f, 5.f, 16.f, 16.f, -55.f, 290.f);
          g->DrawPath(&pen, &arc);
        }
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
    }
  };

  for (int i = 0; i < 6; ++i) {
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

void paint_tab_strip(HWND hwnd, ConsoleState* st) {
  PAINTSTRUCT ps{};
  HDC hdc = BeginPaint(hwnd, &ps);
  RECT rc{};
  GetClientRect(hwnd, &rc);
  HBRUSH chrome = CreateSolidBrush(kChrome);
  FillRect(hdc, &rc, chrome);
  DeleteObject(chrome);
  HGDIOBJ old = st->ui_font ? SelectObject(hdc, st->ui_font) : nullptr;
  SetBkMode(hdc, TRANSPARENT);

  auto draw_catalog = [&](int x) {
    SIZE sz{};
    GetTextExtentPoint32W(hdc, L"目录", 2, &sz);
    const int w = sz.cx + 24;
    RECT tr{x, 2, x + w, rc.bottom - 1};
    if (st->active_tab == kTabCatalog) {
      FillRect(hdc, &tr, reinterpret_cast<HBRUSH>(GetSysColorBrush(COLOR_WINDOW)));
    }
    DrawTextW(hdc, L"目录", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return w;
  };

  auto draw_session = [&](int x, int index) {
    const SessionTab& tab = st->sessions[static_cast<size_t>(index)];
    const std::wstring label = session_tab_label(tab);
    const int w = session_tab_width(hdc, tab);
    RECT tr{x, 2, x + w, rc.bottom - 1};
    if (st->active_tab == index) {
      FillRect(hdc, &tr, reinterpret_cast<HBRUSH>(GetSysColorBrush(COLOR_WINDOW)));
    }
    RECT text_rc = tr;
    text_rc.right -= kCloseBtnW;
    DrawTextW(hdc, label.c_str(), -1, &text_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT close_rc{tr.right - kCloseBtnW - 2, tr.top, tr.right - 2, tr.bottom};
    DrawTextW(hdc, L"×", -1, &close_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return w;
  };

  int cx = 4;
  cx += draw_catalog(cx);
  for (int i = 0; i < static_cast<int>(st->sessions.size()); ++i) {
    cx += draw_session(cx, i);
  }
  if (old) {
    SelectObject(hdc, old);
  }
  EndPaint(hwnd, &ps);
}

LRESULT CALLBACK TabStripProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  ConsoleState* st = g_console;
  switch (msg) {
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
            set_status(st, L"会话已拖出为独立窗口（右键标签可拖回；或关闭后从列表重开）");
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
      // Right-click floating tab → dock back
      if (!st) {
        break;
      }
      int close_idx = -1;
      const int hit = tab_hit_test(st, GET_X_LPARAM(lparam), &close_idx, nullptr);
      if (hit >= 0 && hit < static_cast<int>(st->sessions.size())) {
        SessionTab& tab = st->sessions[static_cast<size_t>(hit)];
        if (tab.floating && tab.host) {
          tab.host->attach_to_parent(st->session_area);
          tab.floating = false;
          select_tab(st, hit);
          set_status(st, L"会话已拖回工作区");
        }
      }
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

HMENU build_placeholder_menu() {
  HMENU menu = CreateMenu();
  HMENU file = CreatePopupMenu();
  AppendMenuW(file, MF_STRING | MF_GRAYED, 1, L"新建连接…");
  AppendMenuW(file, MF_STRING | MF_GRAYED, 2, L"退出");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"文件");
  HMENU view = CreatePopupMenu();
  AppendMenuW(view, MF_STRING | MF_GRAYED, 3, L"刷新");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"查看");
  HMENU help = CreatePopupMenu();
  AppendMenuW(help, MF_STRING | MF_GRAYED, 4, L"关于");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"帮助");
  return menu;
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
      st->ui_font = create_ui_font();
      st->dpi = system_dpi();
      st->toolbar_h = dip(kToolbarHDip, st->dpi);
      st->tab_h = measure_tab_height(st->ui_font);
      SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(st->ui_font), TRUE);

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
      // Square icon buttons spanning full toolbar height; tips via TBN_GETINFOTIP.
      SendMessageW(st->toolbar, TB_SETBUTTONSIZE, 0, MAKELONG(st->toolbar_h, st->toolbar_h));
      st->toolbar_il = build_toolbar_images(dip(kToolbarIconDip, st->dpi));
      SendMessageW(st->toolbar, TB_SETIMAGELIST, 0,
                   reinterpret_cast<LPARAM>(st->toolbar_il));
      {
        const BYTE style = BTNS_BUTTON | BTNS_AUTOSIZE;
        const BYTE check = BTNS_CHECK | BTNS_AUTOSIZE;
        TBBUTTON btns[] = {
            {0, kCmdRefresh, TBSTATE_ENABLED, style, {}, 0, 0},
            {0, 0, TBSTATE_ENABLED, BTNS_SEP, {}, 0, 0},
            {1, kCmdCloseSession, 0, style, {}, 0, 0},
            {2, kCmdCloseAll, 0, style, {}, 0, 0},
            {0, 0, TBSTATE_ENABLED, BTNS_SEP, {}, 0, 0},
            {3, kCmdScreenshot, 0, style, {}, 0, 0},
            {4, kCmdFullscreen, 0, style, {}, 0, 0},
            {5, kCmdViewOnly, 0, check, {}, 0, 0},
        };
        SendMessageW(st->toolbar, TB_ADDBUTTONSW, ARRAYSIZE(btns),
                     reinterpret_cast<LPARAM>(btns));
      }
      SendMessageW(st->toolbar, TB_SETBUTTONSIZE, 0, MAKELONG(st->toolbar_h, st->toolbar_h));
      SendMessageW(st->toolbar, TB_AUTOSIZE, 0, 0);
      SendMessageW(st->toolbar, TB_SETBUTTONSIZE, 0, MAKELONG(st->toolbar_h, st->toolbar_h));

      st->tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | TVS_HASLINES | TVS_LINESATROOT |
                                     TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
                                 0, 0, 100, 100, hwnd, reinterpret_cast<HMENU>(IDC_TREE),
                                 st->instance, nullptr);

      st->tab_strip =
          CreateWindowExW(0, kTabStripClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
                          100, st->tab_h, hwnd, reinterpret_cast<HMENU>(IDC_TABSTRIP),
                          st->instance, nullptr);

      st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
                                     LVS_SHOWSELALWAYS,
                                 0, 0, 100, 100, hwnd, reinterpret_cast<HMENU>(IDC_LIST),
                                 st->instance, nullptr);
      ListView_SetExtendedListViewStyle(st->list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
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

      st->status = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
                                   WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd,
                                   reinterpret_cast<HMENU>(IDC_STATUS), st->instance, nullptr);

      apply_ui_font(st, st->toolbar);
      apply_ui_font(st, st->tree);
      apply_ui_font(st, st->list);
      apply_ui_font(st, st->tab_strip);
      apply_ui_font(st, st->status);
      layout_status_parts(st, 1100);
      set_status(st, catalog_ready_status());

      fill_tree(st);
      layout(st);
      return 0;
    }
    case WM_SIZE:
      layout(st);
      return 0;
    case WM_SESSION_META:
      // Connected: the tab label now carries the live agent version.
      if (st->tab_strip) {
        InvalidateRect(st->tab_strip, nullptr, TRUE);
      }
      return 0;
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
    case WM_COMMAND: {
      const int cmd = LOWORD(wparam);
      if (cmd == kCmdRefresh) {
        refresh_directory(st);
      } else if (cmd == kCmdCloseSession) {
        if (st->active_tab >= 0 && st->active_tab < static_cast<int>(st->sessions.size())) {
          close_session_at(st, st->active_tab);
        }
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
            }
          } else if (cmd == kCmdViewOnly && host) {
            host->set_view_only(!host->view_only());
            update_toolbar_state(st);
            set_status(st, host->view_only() ? L"仅查看模式：键鼠输入已冻结"
                                             : L"已恢复键鼠控制");
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
      if (hdr->hwndFrom == st->toolbar && hdr->code == TBN_GETINFOTIP) {
        auto* info = reinterpret_cast<NMTBGETINFOTIPW*>(lparam);
        const wchar_t* tip = nullptr;
        switch (info->iItem) {
          case kCmdRefresh:
            tip = L"刷新目录";
            break;
          case kCmdCloseSession:
            tip = L"关闭会话";
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
          case kCmdViewOnly:
            tip = L"仅查看（冻结键鼠输入）";
            break;
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
      const int split = st->tree_width;
      if (y > st->toolbar_h && y < rc.bottom - kStatusH && x >= split && x < split + kSplitterW) {
        st->dragging_splitter = true;
        SetCapture(hwnd);
      }
      return 0;
    }
    case WM_MOUSEMOVE:
      if (st->dragging_splitter) {
        st->tree_width = GET_X_LPARAM(lparam);
        layout(st);
      }
      return 0;
    case WM_LBUTTONUP:
      if (st->dragging_splitter) {
        st->dragging_splitter = false;
        ReleaseCapture();
      }
      return 0;
    case WM_DESTROY:
      destroy_drag_ghost(st);
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
      if (st->toolbar_il) {
        ImageList_Destroy(st->toolbar_il);
        st->toolbar_il = nullptr;
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

  HMENU menu = build_placeholder_menu();
  constexpr int kConsoleW = 1100;
  constexpr int kConsoleH = 720;
  HWND hwnd = CreateWindowExW(0, kConsoleClass, L"Road Desk Viewer", WS_OVERLAPPEDWINDOW, 0, 0,
                              kConsoleW, kConsoleH, nullptr, menu, instance, &state);
  if (!hwnd) {
    return 1;
  }
  // Center on the monitor work area. Do not pass WinMain's show_cmd (often SW_SHOWDEFAULT):
  // Explorer STARTUPINFO would otherwise place the window at the cascade top-left.
  RECT rc{};
  GetWindowRect(hwnd, &rc);
  RECT wa{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  const int win_w = rc.right - rc.left;
  const int win_h = rc.bottom - rc.top;
  SetWindowPos(hwnd, nullptr, wa.left + (wa.right - wa.left - win_w) / 2,
               wa.top + (wa.bottom - wa.top - win_h) / 2, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);
  SessionHost::install_keyboard_hook(instance);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  SessionHost::uninstall_keyboard_hook();
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
