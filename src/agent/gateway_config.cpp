#include "gateway_config.h"

#include "log.h"
#include "ui/rd_app_icon.h"
#include "ui/rd_dpi.h"
#include "ui/rd_tokens.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>

#include <gdiplus.h>

namespace road_desk::agent {
namespace {

namespace gdip = Gdiplus;
namespace rd = road_desk::ui;

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
    s.pop_back();
  }
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
    ++i;
  }
  return s.substr(i);
}

std::string json_get_string(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) {
    return {};
  }
  p = json.find(':', p + needle.size());
  if (p == std::string::npos) {
    return {};
  }
  p = json.find('"', p + 1);
  if (p == std::string::npos) {
    return {};
  }
  size_t end = json.find('"', p + 1);
  if (end == std::string::npos) {
    return {};
  }
  return json.substr(p + 1, end - p - 1);
}

std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '\\' || c == '"') {
      o.push_back('\\');
    }
    o.push_back(c);
  }
  return o;
}

std::string new_agent_id() {
  GUID g{};
  if (CoCreateGuid(&g) != S_OK) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "agent-%08lx", GetTickCount());
    return buf;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                static_cast<unsigned long>(g.Data1), g.Data2, g.Data3, g.Data4[0], g.Data4[1],
                g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  return buf;
}

bool split_gateway_url(const std::string& url, std::string* host, std::string* port) {
  if (!host || !port) {
    return false;
  }
  std::string s = url;
  if (s.rfind("http://", 0) == 0) {
    s = s.substr(7);
  } else if (s.rfind("https://", 0) == 0) {
    s = s.substr(8);
  }
  const size_t slash = s.find('/');
  if (slash != std::string::npos) {
    s = s.substr(0, slash);
  }
  const size_t colon = s.rfind(':');
  if (colon == std::string::npos) {
    *host = s;
    *port = "8743";
    return !host->empty();
  }
  *host = s.substr(0, colon);
  *port = s.substr(colon + 1);
  return !host->empty() && !port->empty();
}

// Tokens: docs/ui-design-system.md (Agent config) via ui/rd_tokens.h
constexpr COLORREF kBg = rd::kColorSurfaceApp;
constexpr COLORREF kPanel = rd::kColorSurfacePanel;
constexpr COLORREF kChrome = rd::kColorSurfaceChrome;
constexpr COLORREF kBrandBg = rd::kColorBrandBg;
constexpr COLORREF kBrandMuted = rd::kColorBrandFgMuted;
constexpr COLORREF kLabel = rd::kColorTextSecondary;
constexpr COLORREF kMuted = rd::kColorTextMuted;
constexpr COLORREF kText = rd::kColorTextPrimary;
constexpr COLORREF kRule = rd::kColorBorderSubtle;
constexpr COLORREF kAccent = rd::kColorAccent;
constexpr COLORREF kSuccess = rd::kColorSuccess;
constexpr COLORREF kDanger = rd::kColorDanger;

constexpr int kClientW = 420;
constexpr int kClientH = 384;
constexpr int kHeaderH = rd::kHeaderHDip;
constexpr int kPadX = rd::kSpace7;
constexpr int kPadTop = rd::kSpace5;
constexpr int kEditH = rd::kEditHDip;
constexpr int kLabelH = 18;
constexpr int kFieldGap = rd::kSpace3;
constexpr int kPortW = 120;
constexpr int kBrowseW = 100;
constexpr int kEyeW = 28;
constexpr int kBtnW = 100;
constexpr int kBtnH = 30;
constexpr int kFooterH = 48;
constexpr int kMarkSize = 40;
constexpr int kIconPad = 28;
constexpr int kIDC_BROWSE = 1001;
constexpr int kIDC_EYE = 1002;

enum class LineIcon {
  Server,
  Key,
  Eye,
  EyeOff,
  Folder,
  Cancel,
  Check,
  Alert,
};

enum class StatusKind {
  None,
  Success,
  Error,
};

struct DlgState {
  GatewayConfig* cfg = nullptr;
  HWND host = nullptr;
  HWND port = nullptr;
  HWND psk = nullptr;
  HWND eye = nullptr;
  HWND browse = nullptr;
  HWND ok_btn = nullptr;
  HWND cancel_btn = nullptr;
  HFONT font = nullptr;
  HFONT font_title = nullptr;
  HFONT font_brand = nullptr;
  HFONT font_label = nullptr;
  HFONT font_mark = nullptr;
  HBRUSH bg_brush = nullptr;
  HBRUSH edit_brush = nullptr;
  ULONG_PTR gdiplus_token = 0;
  int dpi = 96;
  int status_y = 0;
  StatusKind status = StatusKind::None;
  std::wstring status_text;
  bool psk_visible = false;
  bool ok = false;
  bool done = false;
};

int dip(const DlgState* st, int v) {
  return rd::rd_dip(v, st && st->dpi > 0 ? st->dpi : 96);
}

DlgState* g_dlg = nullptr;

void set_edit_icon_margin(HWND edit, int left_px) {
  if (edit) {
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN, MAKELPARAM(left_px, 0));
  }
}

gdip::Color gdip_rgb(COLORREF c, BYTE alpha = 255) {
  return gdip::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

void add_round_rect(gdip::GraphicsPath* path, float x, float y, float w, float h, float rx) {
  if (!path || w <= 0 || h <= 0) {
    return;
  }
  rx = (std::min)(rx, (std::min)(w, h) / 2.f);
  const float d = rx * 2.f;
  path->AddArc(x, y, d, d, 180.f, 90.f);
  path->AddArc(x + w - d, y, d, d, 270.f, 90.f);
  path->AddArc(x + w - d, y + h - d, d, d, 0.f, 90.f);
  path->AddArc(x, y + h - d, d, d, 90.f, 90.f);
  path->CloseFigure();
}

void draw_line_icon(HDC hdc, int x, int y, int s, LineIcon id, COLORREF color) {
  if (!hdc || s <= 0) {
    return;
  }
  gdip::Graphics g(hdc);
  g.SetSmoothingMode(gdip::SmoothingModeAntiAlias);
  g.SetPixelOffsetMode(gdip::PixelOffsetModeHalf);
  g.TranslateTransform(static_cast<gdip::REAL>(x), static_cast<gdip::REAL>(y));
  const float scale = static_cast<float>(s) / 24.f;
  g.ScaleTransform(scale, scale);

  gdip::Pen pen(gdip_rgb(color), 2.f);
  pen.SetStartCap(gdip::LineCapRound);
  pen.SetEndCap(gdip::LineCapRound);
  pen.SetLineJoin(gdip::LineJoinRound);
  pen.SetMiterLimit(2.f);

  switch (id) {
    case LineIcon::Server: {
      gdip::GraphicsPath top;
      add_round_rect(&top, 2.f, 2.f, 20.f, 8.f, 2.f);
      g.DrawPath(&pen, &top);
      gdip::GraphicsPath bot;
      add_round_rect(&bot, 2.f, 14.f, 20.f, 8.f, 2.f);
      g.DrawPath(&pen, &bot);
      g.DrawLine(&pen, 6.f, 6.f, 6.01f, 6.f);
      g.DrawLine(&pen, 6.f, 18.f, 6.01f, 18.f);
      break;
    }
    case LineIcon::Key: {
      g.DrawEllipse(&pen, 2.f, 10.f, 11.f, 11.f);
      g.DrawLine(&pen, 21.f, 2.f, 11.4f, 11.6f);
      gdip::PointF tip[] = {{15.5f, 7.5f}, {17.8f, 9.8f}, {19.9f, 7.7f}, {19.f, 4.f}};
      g.DrawLines(&pen, tip, 4);
      break;
    }
    case LineIcon::Eye:
    case LineIcon::EyeOff: {
      gdip::GraphicsPath eye;
      eye.AddBezier(2.f, 12.f, 6.f, 6.5f, 18.f, 6.5f, 22.f, 12.f);
      eye.AddBezier(22.f, 12.f, 18.f, 17.5f, 6.f, 17.5f, 2.f, 12.f);
      g.DrawPath(&pen, &eye);
      g.DrawEllipse(&pen, 9.f, 9.f, 6.f, 6.f);
      if (id == LineIcon::EyeOff) {
        g.DrawLine(&pen, 2.f, 2.f, 22.f, 22.f);
      }
      break;
    }
    case LineIcon::Folder: {
      gdip::GraphicsPath p;
      p.StartFigure();
      p.AddLine(2.f, 20.f, 2.f, 5.f);
      p.AddLine(2.f, 5.f, 4.f, 3.f);
      p.AddLine(4.f, 3.f, 7.93f, 3.f);
      p.AddLine(7.93f, 3.f, 9.6f, 3.9f);
      p.AddLine(9.6f, 3.9f, 11.29f, 6.f);
      p.AddLine(11.29f, 6.f, 20.f, 6.f);
      p.AddArc(18.f, 6.f, 4.f, 4.f, 270.f, 90.f);
      p.AddLine(22.f, 8.f, 22.f, 18.f);
      p.AddArc(18.f, 16.f, 4.f, 4.f, 0.f, 90.f);
      p.AddLine(20.f, 20.f, 4.f, 20.f);
      p.AddArc(2.f, 16.f, 4.f, 4.f, 90.f, 90.f);
      p.AddLine(2.f, 18.f, 2.f, 20.f);
      g.DrawPath(&pen, &p);
      break;
    }
    case LineIcon::Cancel: {
      g.DrawLine(&pen, 18.f, 6.f, 6.f, 18.f);
      g.DrawLine(&pen, 6.f, 6.f, 18.f, 18.f);
      break;
    }
    case LineIcon::Check: {
      gdip::PointF pts[] = {{4.f, 12.f}, {10.f, 18.f}, {20.f, 6.f}};
      g.DrawLines(&pen, pts, 3);
      break;
    }
    case LineIcon::Alert: {
      g.DrawEllipse(&pen, 3.f, 3.f, 18.f, 18.f);
      g.DrawLine(&pen, 12.f, 8.f, 12.f, 13.f);
      g.DrawLine(&pen, 12.f, 16.f, 12.01f, 16.f);
      break;
    }
  }
}

LRESULT CALLBACK icon_edit_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  const auto old = reinterpret_cast<WNDPROC>(GetPropW(hwnd, L"rdOldProc"));
  if (msg == WM_PAINT) {
    const LRESULT r = CallWindowProcW(old, hwnd, msg, wparam, lparam);
    const INT_PTR icon_i = reinterpret_cast<INT_PTR>(GetPropW(hwnd, L"rdIcon"));
    HDC hdc = GetDC(hwnd);
    if (hdc) {
      RECT rc{};
      GetClientRect(hwnd, &rc);
      const int icon_s = dip(g_dlg, 14);
      const int x = dip(g_dlg, 8);
      const int y = (rc.bottom - rc.top - icon_s) / 2;
      draw_line_icon(hdc, x, y, icon_s, static_cast<LineIcon>(icon_i), kMuted);
      ReleaseDC(hwnd, hdc);
    }
    return r;
  }
  if (msg == WM_NCDESTROY) {
    RemovePropW(hwnd, L"rdIcon");
    RemovePropW(hwnd, L"rdOldProc");
  }
  return CallWindowProcW(old, hwnd, msg, wparam, lparam);
}

void attach_edit_icon(HWND edit, LineIcon icon) {
  if (!edit) {
    return;
  }
  SetPropW(edit, L"rdIcon", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(icon)));
  const LONG_PTR prev =
      SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(icon_edit_proc));
  SetPropW(edit, L"rdOldProc", reinterpret_cast<HANDLE>(prev));
}

void set_psk_visible(DlgState* st, bool visible) {
  if (!st || !st->psk) {
    return;
  }
  st->psk_visible = visible;
  SendMessageW(st->psk, EM_SETPASSWORDCHAR, visible ? 0 : static_cast<WPARAM>(0x25CF), 0);
  const int n = GetWindowTextLengthW(st->psk);
  std::wstring buf(static_cast<size_t>(n) + 1, L'\0');
  GetWindowTextW(st->psk, buf.data(), n + 1);
  SetWindowTextW(st->psk, buf.c_str());
  InvalidateRect(st->psk, nullptr, TRUE);
  if (st->eye) {
    InvalidateRect(st->eye, nullptr, TRUE);
  }
}

void set_status(DlgState* st, HWND hwnd, StatusKind kind, const wchar_t* text) {
  if (!st) {
    return;
  }
  st->status = kind;
  st->status_text = text ? text : L"";
  if (hwnd) {
    InvalidateRect(hwnd, nullptr, FALSE);
  }
}

void draw_owner_button(DRAWITEMSTRUCT* dis, DlgState* st, const wchar_t* label, LineIcon icon,
                       bool primary, bool icon_only, bool chrome) {
  if (!dis || !st) {
    return;
  }
  const bool hot = (dis->itemState & ODS_SELECTED) != 0;
  const RECT& rc = dis->rcItem;
  COLORREF bg = primary ? (hot ? rd::kColorAccentHover : kAccent)
                        : (hot ? rd::kColorRowHover : (chrome ? kChrome : kPanel));
  COLORREF fg = primary ? rd::kColorTextOnBrand : kText;
  COLORREF bd = primary ? bg : kRule;
  HBRUSH br = CreateSolidBrush(bg);
  FillRect(dis->hDC, &rc, br);
  DeleteObject(br);
  HPEN pen = CreatePen(PS_SOLID, 1, bd);
  HGDIOBJ old = SelectObject(dis->hDC, pen);
  HGDIOBJ old_br = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
  Rectangle(dis->hDC, rc.left, rc.top, rc.right - 1, rc.bottom - 1);
  SelectObject(dis->hDC, old);
  SelectObject(dis->hDC, old_br);
  DeleteObject(pen);

  const int icon_s = dip(st, 14);
  SetBkMode(dis->hDC, TRANSPARENT);
  SetTextColor(dis->hDC, fg);
  if (icon_only) {
    draw_line_icon(dis->hDC, rc.left + (rc.right - rc.left - icon_s) / 2,
                   rc.top + (rc.bottom - rc.top - icon_s) / 2, icon_s, icon, fg);
    return;
  }
  SIZE tsz{};
  if (st->font_title) {
    SelectObject(dis->hDC, st->font_title);
  }
  GetTextExtentPoint32W(dis->hDC, label, static_cast<int>(wcslen(label)), &tsz);
  const int gap = dip(st, 6);
  const int total = icon_s + gap + tsz.cx;
  int x = rc.left + (rc.right - rc.left - total) / 2;
  const int iy = rc.top + (rc.bottom - rc.top - icon_s) / 2;
  draw_line_icon(dis->hDC, x, iy, icon_s, icon, fg);
  RECT tr{x + icon_s + gap, rc.top, rc.right - dip(st, 4), rc.bottom};
  DrawTextW(dis->hDC, label, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

HFONT make_font(int pt, int weight) {
  return rd::rd_create_font(rd::rd_dpi_screen(), pt, weight);
}

void apply_font(HFONT font, HWND hwnd) {
  if (font && hwnd) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  }
}

void center_on_work_area(HWND hwnd, int win_w, int win_h) {
  RECT wa{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  const int x = wa.left + (wa.right - wa.left - win_w) / 2;
  const int y = wa.top + (wa.bottom - wa.top - win_h) / 2;
  SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void paint_dialog(HWND hwnd, DlgState* st) {
  PAINTSTRUCT ps{};
  HDC hdc = BeginPaint(hwnd, &ps);
  RECT rc{};
  GetClientRect(hwnd, &rc);

  HBRUSH bg = st && st->bg_brush ? st->bg_brush
                                 : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
  FillRect(hdc, &rc, bg);

  const int header_h = dip(st, kHeaderH);
  const int pad_x = dip(st, kPadX);
  const int mark = dip(st, kMarkSize);

  RECT header{0, 0, rc.right, header_h};
  HBRUSH brand_br = CreateSolidBrush(kBrandBg);
  FillRect(hdc, &header, brand_br);
  DeleteObject(brand_br);

  SetBkMode(hdc, TRANSPARENT);

  const int mark_y = (header_h - mark) / 2;
  RECT mark_rc{pad_x, mark_y, pad_x + mark, mark_y + mark};
  HBRUSH accent_br = CreateSolidBrush(kAccent);
  FillRect(hdc, &mark_rc, accent_br);
  DeleteObject(accent_br);
  SetTextColor(hdc, rd::kColorTextOnBrand);
  if (st && st->font_mark) {
    SelectObject(hdc, st->font_mark);
  }
  DrawTextW(hdc, L"RD", -1, &mark_rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

  const int title_x = pad_x + mark + dip(st, 12);
  SetTextColor(hdc, rd::kColorTextOnBrand);
  if (st && st->font_brand) {
    SelectObject(hdc, st->font_brand);
  }
  RECT brand{title_x, mark_y, rc.right - pad_x, mark_y + dip(st, 22)};
  DrawTextW(hdc, L"Road Desk", -1, &brand, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  SetTextColor(hdc, kBrandMuted);
  if (st && st->font_label) {
    SelectObject(hdc, st->font_label);
  }
  RECT sub{title_x, brand.bottom + dip(st, 2), rc.right - pad_x, mark_y + mark};
  DrawTextW(hdc, L"被控端网关配置", -1, &sub, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  if (st && st->status != StatusKind::None && !st->status_text.empty() && st->status_y > 0) {
    const int icon_s = dip(st, 14);
    const COLORREF col = st->status == StatusKind::Success ? kSuccess : kDanger;
    SetTextColor(hdc, col);
    if (st->font_label) {
      SelectObject(hdc, st->font_label);
    }
    draw_line_icon(hdc, pad_x, st->status_y + (dip(st, 18) - icon_s) / 2, icon_s,
                   st->status == StatusKind::Success ? LineIcon::Check : LineIcon::Alert, col);
    RECT tr{pad_x + icon_s + dip(st, 6), st->status_y, rc.right - pad_x, st->status_y + dip(st, 18)};
    DrawTextW(hdc, st->status_text.c_str(), -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  }

  const int div_y = rc.bottom - dip(st, kFooterH);
  HPEN pen = CreatePen(PS_SOLID, 1, kRule);
  HGDIOBJ old = SelectObject(hdc, pen);
  MoveToEx(hdc, pad_x, div_y, nullptr);
  LineTo(hdc, rc.right - pad_x, div_y);
  SelectObject(hdc, old);
  DeleteObject(pen);

  EndPaint(hwnd, &ps);
}

bool apply_dialog(DlgState* st, HWND hwnd) {
  char host[256] = {};
  char port[32] = {};
  char psk[512] = {};
  GetWindowTextA(st->host, host, sizeof(host));
  GetWindowTextA(st->port, port, sizeof(port));
  GetWindowTextA(st->psk, psk, sizeof(psk));
  std::string h = trim(host);
  std::string p = trim(port);
  std::string k = trim(psk);
  if (h.empty() || p.empty() || k.empty()) {
    set_status(st, hwnd, StatusKind::Error, L"请填写网关地址、端口与 Agent PSK");
    return false;
  }
  st->cfg->gateway_url = "http://" + h + ":" + p;
  st->cfg->agent_psk = k;
  if (st->cfg->agent_id.empty()) {
    st->cfg->agent_id = new_agent_id();
  }
  return true;
}

LRESULT CALLBACK CfgWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  DlgState* st = g_dlg;
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      paint_dialog(hwnd, st);
      return 0;
    case WM_DRAWITEM: {
      auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
      if (!st || !dis || dis->CtlType != ODT_BUTTON) {
        break;
      }
      if (dis->CtlID == IDOK) {
        draw_owner_button(dis, st, L"确定", LineIcon::Check, true, false, false);
        return TRUE;
      }
      if (dis->CtlID == IDCANCEL) {
        draw_owner_button(dis, st, L"取消", LineIcon::Cancel, false, false, false);
        return TRUE;
      }
      if (dis->CtlID == kIDC_BROWSE) {
        draw_owner_button(dis, st, L"浏览…", LineIcon::Folder, false, false, true);
        return TRUE;
      }
      if (dis->CtlID == kIDC_EYE) {
        draw_owner_button(dis, st, L"", st->psk_visible ? LineIcon::EyeOff : LineIcon::Eye, false,
                          true, false);
        return TRUE;
      }
      break;
    }
    case WM_CTLCOLORSTATIC:
      if (st) {
        HDC hdc = reinterpret_cast<HDC>(wparam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kLabel);
        return reinterpret_cast<LRESULT>(st->bg_brush);
      }
      break;
    case WM_CTLCOLOREDIT:
      if (st) {
        HDC hdc = reinterpret_cast<HDC>(wparam);
        SetBkColor(hdc, kPanel);
        SetTextColor(hdc, kText);
        return reinterpret_cast<LRESULT>(st->edit_brush);
      }
      break;
    case WM_COMMAND:
      if (!st) {
        break;
      }
      if (LOWORD(wparam) == IDOK) {
        if (!apply_dialog(st, hwnd)) {
          return 0;
        }
        st->ok = true;
        st->done = true;
        DestroyWindow(hwnd);
        return 0;
      }
      if (LOWORD(wparam) == IDCANCEL) {
        st->ok = false;
        st->done = true;
        DestroyWindow(hwnd);
        return 0;
      }
      if (LOWORD(wparam) == kIDC_EYE) {
        set_psk_visible(st, !st->psk_visible);
        return 0;
      }
      if (LOWORD(wparam) == kIDC_BROWSE) {
        char path[MAX_PATH] = {};
        OPENFILENAMEA ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = "PSK Files\0*.psk;*.txt\0All\0*.*\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameA(&ofn)) {
          std::ifstream in(path);
          std::string line;
          std::getline(in, line);
          line = trim(line);
          if (!line.empty()) {
            SetWindowTextA(st->psk, line.c_str());
            set_psk_visible(st, false);
            set_status(st, hwnd, StatusKind::Success, L"已从文件导入 Agent PSK");
          }
        }
        return 0;
      }
      break;
    case WM_CLOSE:
      if (st) {
        st->ok = false;
        st->done = true;
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      if (st) {
        if (st->font) {
          DeleteObject(st->font);
          st->font = nullptr;
        }
        if (st->font_title) {
          DeleteObject(st->font_title);
          st->font_title = nullptr;
        }
        if (st->font_brand) {
          DeleteObject(st->font_brand);
          st->font_brand = nullptr;
        }
        if (st->font_label) {
          DeleteObject(st->font_label);
          st->font_label = nullptr;
        }
        if (st->font_mark) {
          DeleteObject(st->font_mark);
          st->font_mark = nullptr;
        }
        if (st->bg_brush) {
          DeleteObject(st->bg_brush);
          st->bg_brush = nullptr;
        }
        if (st->edit_brush) {
          DeleteObject(st->edit_brush);
          st->edit_brush = nullptr;
        }
        if (st->gdiplus_token) {
          gdip::GdiplusShutdown(st->gdiplus_token);
          st->gdiplus_token = 0;
        }
      }
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

std::string gateway_config_path() {
  char prog[MAX_PATH] = {};
  if (FAILED(SHGetFolderPathA(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, prog))) {
    return "agent.json";
  }
  return std::string(prog) + "\\RoadDesk\\agent.json";
}

bool load_gateway_config(GatewayConfig* out) {
  if (!out) {
    return false;
  }
  const std::string path = gateway_config_path();
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  out->agent_id = json_get_string(json, "agentId");
  out->gateway_url = json_get_string(json, "gatewayUrl");
  out->agent_psk = json_get_string(json, "agentPsk");
  return !out->agent_id.empty() && !out->gateway_url.empty() && !out->agent_psk.empty();
}

bool save_gateway_config(const GatewayConfig& cfg) {
  const std::string path = gateway_config_path();
  const size_t slash = path.find_last_of("\\/");
  if (slash != std::string::npos) {
    CreateDirectoryA(path.substr(0, slash).c_str(), nullptr);
  }
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    log_line("gateway: failed to write agent.json");
    return false;
  }
  out << "{\n"
      << "  \"agentId\": \"" << json_escape(cfg.agent_id) << "\",\n"
      << "  \"gatewayUrl\": \"" << json_escape(cfg.gateway_url) << "\",\n"
      << "  \"agentPsk\": \"" << json_escape(cfg.agent_psk) << "\"\n"
      << "}\n";
  return true;
}

bool prompt_gateway_config(GatewayConfig* out) {
  if (!out) {
    return false;
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  std::string pre_host = "127.0.0.1";
  std::string pre_port = "8743";
  if (!out->gateway_url.empty()) {
    std::string h, p;
    if (split_gateway_url(out->gateway_url, &h, &p)) {
      pre_host = h;
      pre_port = p;
    }
  }

  const wchar_t* kClass = L"RoadDeskAgentGatewayCfg";
  WNDCLASSW wc{};
  wc.lpfnWndProc = CfgWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kClass;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  rd::rd_apply_wndclass_icons(&wc, wc.hInstance);
  RegisterClassW(&wc);

  HDC screen = GetDC(nullptr);
  const int dpi = GetDeviceCaps(screen, LOGPIXELSY);
  ReleaseDC(nullptr, screen);

  DlgState st;
  st.cfg = out;
  st.dpi = dpi > 0 ? dpi : 96;
  st.font = make_font(rd::kFontBodyPt, FW_NORMAL);
  st.font_title = make_font(rd::kFontBodyPt, FW_SEMIBOLD);
  st.font_brand = make_font(rd::kFontBrandPt, FW_SEMIBOLD);
  st.font_label = make_font(rd::kFontLabelPt, FW_NORMAL);
  st.font_mark = make_font(13, FW_BOLD);
  st.bg_brush = CreateSolidBrush(kBg);
  st.edit_brush = CreateSolidBrush(kPanel);

  gdip::GdiplusStartupInput gdiplus_input;
  if (gdip::GdiplusStartup(&st.gdiplus_token, &gdiplus_input, nullptr) != gdip::Ok) {
    st.gdiplus_token = 0;
  }

  g_dlg = &st;

  const int client_w = dip(&st, kClientW);
  const int client_h = dip(&st, kClientH);
  RECT wr{0, 0, client_w, client_h};
  const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
  const DWORD ex_style = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
  AdjustWindowRectEx(&wr, style, FALSE, ex_style);
  const int win_w = wr.right - wr.left;
  const int win_h = wr.bottom - wr.top;

  HWND hwnd = CreateWindowExW(ex_style, kClass, L"Road Desk — 网关配置", style, 0, 0, win_w, win_h,
                              nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) {
    if (st.gdiplus_token) {
      gdip::GdiplusShutdown(st.gdiplus_token);
      st.gdiplus_token = 0;
    }
    g_dlg = nullptr;
    return false;
  }
  rd::rd_set_window_icons(hwnd, wc.hInstance);
  center_on_work_area(hwnd, win_w, win_h);

  const int pad_x = dip(&st, kPadX);
  const int field_w = client_w - pad_x * 2;
  const int edit_h = dip(&st, kEditH);
  const int label_h = dip(&st, kLabelH);
  const int port_w = dip(&st, kPortW);
  const int browse_w = dip(&st, kBrowseW);
  const int eye_w = dip(&st, kEyeW);
  const int icon_m = dip(&st, kIconPad);
  const int gap = dip(&st, 8);
  const int btn_w = dip(&st, kBtnW);
  const int btn_h = dip(&st, kBtnH);
  int y = dip(&st, kHeaderH + kPadTop);

  auto label = [&](const wchar_t* text, int yy, int x, int w) {
    HWND s = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, yy, w, label_h, hwnd, nullptr,
                           wc.hInstance, nullptr);
    apply_font(st.font_label, s);
  };

  label(L"网关 IP / 主机", y, pad_x, field_w);
  y += label_h + dip(&st, 6);
  st.host = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", pre_host.c_str(),
                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, pad_x, y, field_w,
                            edit_h, hwnd, nullptr, wc.hInstance, nullptr);
  apply_font(st.font, st.host);
  set_edit_icon_margin(st.host, icon_m);
  attach_edit_icon(st.host, LineIcon::Server);
  y += edit_h + dip(&st, kFieldGap);

  label(L"端口", y, pad_x, port_w);
  y += label_h + dip(&st, 6);
  st.port = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", pre_port.c_str(),
                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP, pad_x,
                            y, port_w, edit_h, hwnd, nullptr, wc.hInstance, nullptr);
  apply_font(st.font, st.port);
  y += edit_h + dip(&st, kFieldGap);

  label(L"Agent PSK", y, pad_x, field_w);
  y += label_h + dip(&st, 6);
  const int psk_w = field_w - eye_w - browse_w - gap * 2;
  st.psk = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, pad_x, y, psk_w,
                           edit_h, hwnd, nullptr, wc.hInstance, nullptr);
  st.eye = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                         pad_x + psk_w + gap, y, eye_w, edit_h, hwnd,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIDC_EYE)), wc.hInstance,
                         nullptr);
  st.browse = CreateWindowW(L"BUTTON", L"浏览…", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                            pad_x + psk_w + gap + eye_w + gap, y, browse_w, edit_h, hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIDC_BROWSE)),
                            wc.hInstance, nullptr);
  apply_font(st.font, st.psk);
  set_edit_icon_margin(st.psk, icon_m);
  attach_edit_icon(st.psk, LineIcon::Key);
  set_psk_visible(&st, false);
  if (!out->agent_psk.empty()) {
    SetWindowTextA(st.psk, out->agent_psk.c_str());
  }
  y += edit_h + dip(&st, kFieldGap);
  st.status_y = y;

  const int footer_top = client_h - dip(&st, kFooterH);
  const int btn_y = footer_top + (dip(&st, kFooterH) - btn_h) / 2;
  st.cancel_btn =
      CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                    client_w - pad_x - btn_w * 2 - gap, btn_y, btn_w, btn_h, hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), wc.hInstance, nullptr);
  st.ok_btn =
      CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_DEFPUSHBUTTON |
                                            WS_TABSTOP,
                    client_w - pad_x - btn_w, btn_y, btn_w, btn_h, hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), wc.hInstance, nullptr);

  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);
  SetFocus(st.host);

  MSG msg;
  while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  g_dlg = nullptr;
  return st.ok;
}

}  // namespace road_desk::agent
