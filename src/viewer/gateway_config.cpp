#include "gateway_config.h"

#include "common/http_helpers.h"
#include "product_version.h"
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
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <winhttp.h>
#include <gdiplus.h>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace gdip = Gdiplus;

namespace road_desk::viewer {
namespace {

namespace rd = road_desk::ui;

bool looks_like_jwt(const std::string& k) {
  // HS256 JWT: header.payload.signature (exactly two dots).
  const size_t d1 = k.find('.');
  if (d1 == std::string::npos || d1 == 0) {
    return false;
  }
  const size_t d2 = k.find('.', d1 + 1);
  if (d2 == std::string::npos || d2 + 1 >= k.size()) {
    return false;
  }
  return k.find('.', d2 + 1) == std::string::npos;
}

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

std::wstring utf8_to_wide(const std::string& s) {
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

std::string env_str(const char* name) {
  char* env = nullptr;
  size_t len = 0;
  if (_dupenv_s(&env, &len, name) != 0 || !env) {
    return {};
  }
  std::string s(env);
  free(env);
  return s;
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
  size_t end = p + 1;
  while (end < json.size()) {
    if (json[end] == '\\' && end + 1 < json.size()) {
      end += 2;
      continue;
    }
    if (json[end] == '"') {
      break;
    }
    ++end;
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

bool json_get_bool(const std::string& json, const char* key, bool default_val = false) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) {
    return default_val;
  }
  p = json.find(':', p + needle.size());
  if (p == std::string::npos) {
    return default_val;
  }
  ++p;
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) {
    ++p;
  }
  if (json.compare(p, 4, "true") == 0) {
    return true;
  }
  if (json.compare(p, 5, "false") == 0) {
    return false;
  }
  return default_val;
}

bool parse_url(const std::string& url, std::wstring* host, INTERNET_PORT* port, bool* https,
               std::wstring* path_prefix) {
  URL_COMPONENTSW uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t host_buf[256] = {};
  wchar_t path_buf[512] = {};
  uc.lpszHostName = host_buf;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path_buf;
  uc.dwUrlPathLength = 512;
  std::wstring wurl(url.begin(), url.end());
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    return false;
  }
  *host = host_buf;
  *port = uc.nPort;
  *https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
  *path_prefix = path_buf;
  while (!path_prefix->empty() && path_prefix->back() == L'/') {
    path_prefix->pop_back();
  }
  return !host->empty();
}

bool split_gateway_url(const std::string& url, std::string* host, std::string* port) {
  std::wstring whost;
  unsigned short p = 0;
  bool https = false;
  std::wstring prefix;
  if (!road_desk::common::parse_url(url, &whost, &p, &https, &prefix)) {
    return false;
  }
  char narrow[256] = {};
  WideCharToMultiByte(CP_UTF8, 0, whost.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
  *host = narrow;
  char port_buf[16];
  std::snprintf(port_buf, sizeof(port_buf), "%u", static_cast<unsigned>(p));
  *port = port_buf;
  return true;
}

bool http_post_json(const std::string& base_url, const std::string& path, const std::string& json_body,
                    std::string* body, std::string* err) {
  std::wstring host;
  INTERNET_PORT port = 0;
  bool https = false;
  std::wstring prefix;
  if (!parse_url(base_url, &host, &port, &https, &prefix)) {
    *err = "bad gateway url";
    return false;
  }
  HINTERNET ses = WinHttpOpen(L"RoadDesk-Viewer/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) {
    *err = "WinHttpOpen failed";
    return false;
  }
  HINTERNET con = WinHttpConnect(ses, host.c_str(), port, 0);
  if (!con) {
    WinHttpCloseHandle(ses);
    *err = "connect failed";
    return false;
  }
  std::wstring wpath = prefix + std::wstring(path.begin(), path.end());
  DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET req = WinHttpOpenRequest(con, L"POST", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!req) {
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    *err = "open request failed";
    return false;
  }
  std::string payload = json_body;
  BOOL ok = WinHttpSendRequest(req, L"Content-Type: application/json\r\n", static_cast<DWORD>(-1),
                               payload.empty() ? WINHTTP_NO_REQUEST_DATA : payload.data(),
                               static_cast<DWORD>(payload.size()), static_cast<DWORD>(payload.size()),
                               0);
  if (ok) {
    ok = WinHttpReceiveResponse(req, nullptr);
  }
  DWORD status = 0;
  DWORD status_len = sizeof(status);
  if (ok) {
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len, WINHTTP_NO_HEADER_INDEX);
  }
  std::string out;
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) {
      break;
    }
    std::string chunk(avail, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(req, chunk.data(), avail, &read)) {
      break;
    }
    chunk.resize(read);
    out += chunk;
  }
  WinHttpCloseHandle(req);
  WinHttpCloseHandle(con);
  WinHttpCloseHandle(ses);
  if (!ok || status < 200 || status >= 300) {
    std::string msg = json_get_string(out, "error");
    if (msg.empty()) {
      char line[64];
      std::snprintf(line, sizeof(line), "HTTP %lu", static_cast<unsigned long>(status));
      msg = line;
    }
    *err = msg;
    return false;
  }
  *body = std::move(out);
  return true;
}

// Tokens: docs/ui-design-system.md (Viewer login) via ui/rd_tokens.h
constexpr COLORREF kBg = rd::kColorSurfaceApp;
constexpr COLORREF kPanel = rd::kColorSurfacePanel;
constexpr COLORREF kBrandBg = rd::kColorBrandBg;
constexpr COLORREF kBrandMuted = rd::kColorBrandFgMuted;
constexpr COLORREF kLabel = rd::kColorTextSecondary;
constexpr COLORREF kMuted = rd::kColorTextMuted;
constexpr COLORREF kText = rd::kColorTextPrimary;
constexpr COLORREF kRule = rd::kColorBorderSubtle;
constexpr COLORREF kAccent = rd::kColorAccent;
constexpr COLORREF kSuccess = rd::kColorSuccess;

// Layout DIPs — docs/ui-design-system.md § Viewer login
constexpr int kClientW = 800;
constexpr int kLeftW = 320;
constexpr int kPadBrand = 36;
constexpr int kPadFormX = 40;
constexpr int kPadFormTop = 36;
constexpr int kEditH = 32;
constexpr int kLabelH = 18;
constexpr int kFieldGap = rd::kSpace4;
constexpr int kPortW = 100;
constexpr int kBrowseW = 110;
constexpr int kEyeW = rd::kSpace8;
constexpr int kBtnH = 36;
constexpr int kBtnCancelW = 100;
constexpr int kMarkSize = 48;
constexpr int kIconPad = 28;  // left text margin inside edits for prefix icon
constexpr int kIDC_BROWSE = 1001;
constexpr int kIDC_EYE = 1002;
constexpr int kIDC_REMEMBER = 1003;
constexpr int kCheckH = 20;

enum class LineIcon {
  Server,
  User,
  Lock,
  Key,
  Eye,
  EyeOff,
  Folder,
  Login,
  Cancel,
};

struct DlgState {
  DirectoryConfig* cfg = nullptr;
  HWND host = nullptr;
  HWND port = nullptr;
  HWND user = nullptr;
  HWND pass = nullptr;
  HWND remember = nullptr;
  HWND psk = nullptr;
  HWND eye = nullptr;
  HWND browse = nullptr;
  HWND ok_btn = nullptr;
  HWND cancel_btn = nullptr;
  HFONT font = nullptr;
  HFONT font_title = nullptr;
  HFONT font_brand = nullptr;
  HFONT font_login = nullptr;
  HFONT font_label = nullptr;
  HFONT font_mark = nullptr;
  HBRUSH bg_brush = nullptr;
  HBRUSH panel_brush = nullptr;
  HBRUSH edit_brush = nullptr;
  gdip::Bitmap* brand_img = nullptr;
  ULONG_PTR gdiplus_token = 0;
  int dpi = 96;
  int sep_y = 0;
  bool pass_visible = false;
  bool psk_imported = false;
  bool ok = false;
  bool done = false;
};

int dip(const DlgState* st, int v) {
  return rd::rd_dip(v, st && st->dpi > 0 ? st->dpi : 96);
}

DlgState* g_dlg = nullptr;

std::wstring module_dir() {
  wchar_t mod[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, mod, MAX_PATH);
  std::wstring dir(mod);
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    dir.resize(slash + 1);
  }
  return dir;
}

std::wstring find_brand_image() {
  const std::wstring dir = module_dir();
  const wchar_t* rels[] = {
      L"assets\\brand\\rd_login_brand.png",
      L"..\\assets\\brand\\rd_login_brand.png",
      L"..\\..\\assets\\brand\\rd_login_brand.png",
      L"..\\..\\..\\assets\\brand\\rd_login_brand.png",
      L"..\\..\\..\\..\\assets\\brand\\rd_login_brand.png",
  };
  for (const wchar_t* rel : rels) {
    const std::wstring path = dir + rel;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return path;
    }
  }
  if (GetFileAttributesW(L"assets\\brand\\rd_login_brand.png") != INVALID_FILE_ATTRIBUTES) {
    return L"assets\\brand\\rd_login_brand.png";
  }
  return {};
}

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

// Lucide 24×24 stroke icons (stroke-width 2, round caps/joins) — matches design lucide set.
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
    case LineIcon::User: {
      g.DrawEllipse(&pen, 8.f, 3.f, 8.f, 8.f);
      gdip::GraphicsPath body;
      body.StartFigure();
      body.AddLine(5.f, 21.f, 5.f, 19.f);
      body.AddArc(5.f, 15.f, 8.f, 8.f, 180.f, 90.f);   // (5,19) -> (9,15)
      body.AddLine(9.f, 15.f, 15.f, 15.f);
      body.AddArc(11.f, 15.f, 8.f, 8.f, 270.f, 90.f);  // (15,15) -> (19,19)
      body.AddLine(19.f, 19.f, 19.f, 21.f);
      g.DrawPath(&pen, &body);
      break;
    }
    case LineIcon::Lock: {
      gdip::GraphicsPath box;
      add_round_rect(&box, 3.f, 11.f, 18.f, 11.f, 2.f);
      g.DrawPath(&pen, &box);
      gdip::GraphicsPath shackle;
      shackle.StartFigure();
      shackle.AddLine(7.f, 11.f, 7.f, 7.f);
      shackle.AddArc(7.f, 2.f, 10.f, 10.f, 180.f, 180.f);  // (7,7)->(17,7) over top
      shackle.AddLine(17.f, 7.f, 17.f, 11.f);
      g.DrawPath(&pen, &shackle);
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
      p.AddArc(18.f, 6.f, 4.f, 4.f, 270.f, 90.f);  // (20,6)->(22,8)
      p.AddLine(22.f, 8.f, 22.f, 18.f);
      p.AddArc(18.f, 16.f, 4.f, 4.f, 0.f, 90.f);    // (22,18)->(20,20)
      p.AddLine(20.f, 20.f, 4.f, 20.f);
      p.AddArc(2.f, 16.f, 4.f, 4.f, 90.f, 90.f);     // (4,20)->(2,18) then close up left
      p.AddLine(2.f, 18.f, 2.f, 20.f);
      g.DrawPath(&pen, &p);
      break;
    }
    case LineIcon::Login: {
      gdip::GraphicsPath door;
      door.StartFigure();
      door.AddLine(15.f, 3.f, 19.f, 3.f);
      door.AddArc(17.f, 3.f, 4.f, 4.f, 270.f, 90.f);
      door.AddLine(21.f, 5.f, 21.f, 19.f);
      door.AddArc(17.f, 17.f, 4.f, 4.f, 0.f, 90.f);
      door.AddLine(19.f, 21.f, 15.f, 21.f);
      g.DrawPath(&pen, &door);
      gdip::PointF arrow[] = {{10.f, 17.f}, {15.f, 12.f}, {10.f, 7.f}};
      g.DrawLines(&pen, arrow, 3);
      g.DrawLine(&pen, 15.f, 12.f, 3.f, 12.f);
      break;
    }
    case LineIcon::Cancel: {
      g.DrawLine(&pen, 18.f, 6.f, 6.f, 18.f);
      g.DrawLine(&pen, 6.f, 6.f, 18.f, 18.f);
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
      const int icon_s = dip(g_dlg, 16);
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

void set_password_visible(DlgState* st, bool visible) {
  if (!st || !st->pass) {
    return;
  }
  st->pass_visible = visible;
  SendMessageW(st->pass, EM_SETPASSWORDCHAR, visible ? 0 : static_cast<WPARAM>(0x25CF), 0);
  const int n = GetWindowTextLengthW(st->pass);
  std::wstring buf(static_cast<size_t>(n) + 1, L'\0');
  GetWindowTextW(st->pass, buf.data(), n + 1);
  SetWindowTextW(st->pass, buf.c_str());
  InvalidateRect(st->pass, nullptr, TRUE);
  if (st->eye) {
    InvalidateRect(st->eye, nullptr, TRUE);
  }
}

void draw_owner_button(DRAWITEMSTRUCT* dis, DlgState* st, const wchar_t* label, LineIcon icon,
                       bool primary, bool icon_only) {
  if (!dis || !st) {
    return;
  }
  const bool hot = (dis->itemState & ODS_SELECTED) != 0;
  const RECT& rc = dis->rcItem;
  COLORREF bg = primary ? (hot ? rd::kColorAccentHover : kAccent) : (hot ? rd::kColorSurfaceChrome : kPanel);
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

  const int icon_s = dip(st, 16);
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

bool apply_dialog(DlgState* st, HWND hwnd) {
  char host[256] = {};
  char port[32] = {};
  char psk[2048] = {};
  wchar_t user_w[128] = {};
  wchar_t pass_w[256] = {};
  GetWindowTextA(st->host, host, sizeof(host));
  GetWindowTextA(st->port, port, sizeof(port));
  GetWindowTextW(st->user, user_w, 128);
  GetWindowTextW(st->pass, pass_w, 256);
  if (st->psk) {
    GetWindowTextA(st->psk, psk, sizeof(psk));
  }
  std::string h = trim(host);
  std::string p = trim(port);
  char user_u8[384] = {};
  char pass_u8[768] = {};
  WideCharToMultiByte(CP_UTF8, 0, user_w, -1, user_u8, sizeof(user_u8), nullptr, nullptr);
  WideCharToMultiByte(CP_UTF8, 0, pass_w, -1, pass_u8, sizeof(pass_u8), nullptr, nullptr);
  std::string u = trim(user_u8);
  std::string pw = trim(pass_u8);
  std::string k = trim(psk);
  if (h.empty() || p.empty()) {
    MessageBoxW(hwnd, L"请填写网关地址与端口", L"Road Desk", MB_OK | MB_ICONWARNING);
    return false;
  }
  const std::string url = "http://" + h + ":" + p;
  const bool remember =
      st->remember && SendMessageW(st->remember, BM_GETCHECK, 0, 0) == BST_CHECKED;
  auto store_remember = [&]() {
    st->cfg->remember_password = remember;
    if (remember && !pw.empty()) {
      st->cfg->password = pw;
    } else {
      st->cfg->password.clear();
    }
  };
  if (!k.empty()) {
    st->cfg->gateway_url = url;
    st->cfg->directory_key = k;
    st->cfg->viewer_psk = k;
    if (!u.empty()) {
      st->cfg->username = u;
    }
    store_remember();
    return true;
  }
  if (!u.empty() && !pw.empty()) {
    std::string token;
    std::string err;
    if (!viewer_login(url, u, pw, &token, &err)) {
      MessageBoxW(hwnd, (L"登录失败：" + utf8_to_wide(err)).c_str(), L"Road Desk",
                  MB_OK | MB_ICONERROR);
      return false;
    }
    st->cfg->gateway_url = url;
    st->cfg->directory_key = token;
    st->cfg->username = u;
    store_remember();
    return true;
  }
  if (!st->cfg->viewer_psk.empty()) {
    st->cfg->gateway_url = url;
    st->cfg->directory_key = st->cfg->viewer_psk;
    if (!u.empty()) {
      st->cfg->username = u;
    }
    store_remember();
    return true;
  }
  MessageBoxW(hwnd, L"请填写帐号密码，或导入 Viewer PSK", L"Road Desk", MB_OK | MB_ICONWARNING);
  return false;
}

void paint_login(HWND hwnd, DlgState* st) {
  PAINTSTRUCT ps{};
  HDC hdc = BeginPaint(hwnd, &ps);
  RECT rc{};
  GetClientRect(hwnd, &rc);

  const int left_w = dip(st, kLeftW);
  const int pad_b = dip(st, kPadBrand);
  const int pad_fx = dip(st, kPadFormX);
  const int mark = dip(st, kMarkSize);

  HBRUSH panel = st && st->panel_brush ? st->panel_brush
                                       : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
  FillRect(hdc, &rc, panel);

  RECT left{0, 0, left_w, rc.bottom};
  bool drew_img = false;
  if (st && st->brand_img && st->brand_img->GetLastStatus() == gdip::Ok) {
    gdip::Graphics g(hdc);
    g.SetInterpolationMode(gdip::InterpolationModeHighQualityBicubic);
    g.DrawImage(st->brand_img, 0, 0, left_w, rc.bottom);
    gdip::SolidBrush scrim(gdip::Color(160, 22, 30, 46));
    g.FillRectangle(&scrim, 0, 0, left_w, rc.bottom);
    drew_img = true;
  }
  if (!drew_img) {
    HBRUSH brand_br = CreateSolidBrush(kBrandBg);
    FillRect(hdc, &left, brand_br);
    DeleteObject(brand_br);
  }

  SetBkMode(hdc, TRANSPARENT);

  // Brand mark block.
  RECT mark_rc{pad_b, pad_b, pad_b + mark, pad_b + mark};
  HBRUSH accent_br = CreateSolidBrush(kAccent);
  FillRect(hdc, &mark_rc, accent_br);
  DeleteObject(accent_br);
  SetTextColor(hdc, rd::kColorTextOnBrand);
  if (st && st->font_mark) {
    SelectObject(hdc, st->font_mark);
  }
  DrawTextW(hdc, L"RD", -1, &mark_rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

  SetTextColor(hdc, rd::kColorTextOnBrand);
  if (st && st->font_brand) {
    SelectObject(hdc, st->font_brand);
  }
  RECT brand{pad_b, pad_b + mark + dip(st, 20), left_w - pad_b, pad_b + mark + dip(st, 48)};
  DrawTextW(hdc, L"Road Desk", -1, &brand, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  SetTextColor(hdc, kBrandMuted);
  if (st && st->font) {
    SelectObject(hdc, st->font);
  }
  RECT sub{pad_b, brand.bottom + dip(st, 4), left_w - pad_b, brand.bottom + dip(st, 28)};
  DrawTextW(hdc, L"操作端登录", -1, &sub, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  if (st && st->font_label) {
    SelectObject(hdc, st->font_label);
  }
  RECT slogan{pad_b, rc.bottom - pad_b - dip(st, 48), left_w - pad_b, rc.bottom - pad_b};
  DrawTextW(hdc, L"连接目录服务，拉取被控端列表。\n远控画面由操作端直连被控端。", -1, &slogan,
            DT_LEFT | DT_WORDBREAK);

  // Form chrome on the right.
  const int form_l = left_w + pad_fx;
  const int form_r = rc.right - pad_fx;

  SetTextColor(hdc, kText);
  if (st && st->font_login) {
    SelectObject(hdc, st->font_login);
  }
  RECT welcome{form_l, dip(st, kPadFormTop), form_r, dip(st, kPadFormTop + 28)};
  DrawTextW(hdc, L"欢迎登录", -1, &welcome, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  SetTextColor(hdc, kMuted);
  if (st && st->font_label) {
    SelectObject(hdc, st->font_label);
  }
  RECT tip{form_l, welcome.bottom + dip(st, 4), form_r, welcome.bottom + dip(st, 24)};
  DrawTextW(hdc, L"登录后进入管理台，管理被控端目录与会话。", -1, &tip,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  if (st && !st->psk_imported && st->psk && st->sep_y > 0) {
    const int mid_y = st->sep_y;
    const wchar_t* sep = L"  或导入 Viewer PSK  ";
    SIZE sz{};
    GetTextExtentPoint32W(hdc, sep, static_cast<int>(wcslen(sep)), &sz);

    HPEN pen = CreatePen(PS_SOLID, 1, kRule);
    HGDIOBJ old = SelectObject(hdc, pen);
    const int gap = dip(st, 10);
    const int text_l = form_l + (form_r - form_l - sz.cx) / 2;
    const int text_r = text_l + sz.cx;
    MoveToEx(hdc, form_l, mid_y, nullptr);
    LineTo(hdc, text_l - gap, mid_y);
    MoveToEx(hdc, text_r + gap, mid_y, nullptr);
    LineTo(hdc, form_r, mid_y);
    SelectObject(hdc, old);
    DeleteObject(pen);

    SetTextColor(hdc, kMuted);
    RECT label{text_l, mid_y - dip(st, 10), text_r, mid_y + dip(st, 10)};
    DrawTextW(hdc, sep, -1, &label, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  } else if (st && st->psk_imported) {
    SetTextColor(hdc, kSuccess);
    RECT hint{form_l, st->sep_y - dip(st, 10), form_r, st->sep_y + dip(st, 10)};
    DrawTextW(hdc, L"已保存 Viewer PSK，可直接登录或改用帐号", -1, &hint,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  }

  // Version — form footer strip below buttons, right-aligned.
  {
    wchar_t ver[64] = {};
    _snwprintf_s(ver, _TRUNCATE, L"v%hs", ROAD_DESK_VERSION_STRING);
    SetTextColor(hdc, kMuted);
    if (st && st->font_label) {
      SelectObject(hdc, st->font_label);
    }
    RECT ver_rc{form_l, rc.bottom - dip(st, 22), form_r, rc.bottom - dip(st, 4)};
    DrawTextW(hdc, ver, -1, &ver_rc, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
  }

  EndPaint(hwnd, &ps);
}

LRESULT CALLBACK CfgWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  DlgState* st = g_dlg;
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      paint_login(hwnd, st);
      return 0;
    case WM_DRAWITEM: {
      auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
      if (!st || !dis || dis->CtlType != ODT_BUTTON) {
        break;
      }
      if (dis->CtlID == IDOK) {
        draw_owner_button(dis, st, L"登录", LineIcon::Login, true, false);
        return TRUE;
      }
      if (dis->CtlID == IDCANCEL) {
        draw_owner_button(dis, st, L"取消", LineIcon::Cancel, false, false);
        return TRUE;
      }
      if (dis->CtlID == kIDC_BROWSE) {
        draw_owner_button(dis, st, L"浏览…", LineIcon::Folder, false, false);
        return TRUE;
      }
      if (dis->CtlID == kIDC_EYE) {
        draw_owner_button(dis, st, L"", st->pass_visible ? LineIcon::EyeOff : LineIcon::Eye, false,
                          true);
        return TRUE;
      }
      break;
    }
    case WM_CTLCOLORSTATIC:
      if (st) {
        HDC hdc = reinterpret_cast<HDC>(wparam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kLabel);
        return reinterpret_cast<LRESULT>(st->panel_brush ? st->panel_brush : st->bg_brush);
      }
      break;
    case WM_CTLCOLORBTN:
      if (st) {
        HDC hdc = reinterpret_cast<HDC>(wparam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kLabel);
        return reinterpret_cast<LRESULT>(st->panel_brush ? st->panel_brush : st->bg_brush);
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
        set_password_visible(st, !st->pass_visible);
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
        if (st->font_login) {
          DeleteObject(st->font_login);
          st->font_login = nullptr;
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
        if (st->panel_brush) {
          DeleteObject(st->panel_brush);
          st->panel_brush = nullptr;
        }
        if (st->edit_brush) {
          DeleteObject(st->edit_brush);
          st->edit_brush = nullptr;
        }
        if (st->brand_img) {
          delete st->brand_img;
          st->brand_img = nullptr;
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

std::string directory_config_path() {
  char prog[MAX_PATH] = {};
  if (FAILED(SHGetFolderPathA(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, prog))) {
    return "viewer.json";
  }
  return std::string(prog) + "\\RoadDesk\\viewer.json";
}

DirectoryConfig load_directory_config() {
  DirectoryConfig c;
  c.gateway_url = env_str("ROAD_DESK_GATEWAY_URL");
  // Session key from env only — disk never restores a login session.
  c.directory_key = env_str("ROAD_DESK_VIEWER_KEY");
  if (c.directory_key.empty()) {
    c.directory_key = env_str("ROAD_DESK_DIRECTORY_KEY");
  }
  c.username = env_str("ROAD_DESK_VIEWER_USER");
  if (!c.directory_key.empty() && !looks_like_jwt(c.directory_key)) {
    c.viewer_psk = c.directory_key;
  }

  const std::string path = directory_config_path();
  std::ifstream in(path);
  if (in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string json = ss.str();
    if (c.gateway_url.empty()) {
      c.gateway_url = json_get_string(json, "gatewayUrl");
    }
    if (c.username.empty()) {
      c.username = json_get_string(json, "username");
    }
    c.remember_password = json_get_bool(json, "rememberPassword", false);
    if (c.remember_password) {
      c.password = json_get_string(json, "password");
    }
    if (c.viewer_psk.empty()) {
      std::string psk = json_get_string(json, "viewerPsk");
      if (psk.empty()) {
        // Legacy: viewerKey/directoryKey held PSK or JWT; keep PSK only.
        psk = json_get_string(json, "viewerKey");
        if (psk.empty()) {
          psk = json_get_string(json, "directoryKey");
        }
        if (!psk.empty() && looks_like_jwt(psk)) {
          psk.clear();
        }
      }
      c.viewer_psk = std::move(psk);
    }
    c.viewer_instance_id = json_get_string(json, "viewerInstanceId");
  }
  return c;
}

bool save_directory_config(const DirectoryConfig& cfg) {
  const std::string path = directory_config_path();
  const size_t slash = path.find_last_of("\\/");
  if (slash != std::string::npos) {
    CreateDirectoryA(path.substr(0, slash).c_str(), nullptr);
  }
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return false;
  }
  // Persist gateway + imported PSK + username. Password only when rememberPassword.
  // Never persist JWT session tokens.
  out << "{\n"
      << "  \"gatewayUrl\": \"" << json_escape(cfg.gateway_url) << "\"";
  if (!cfg.viewer_psk.empty()) {
    out << ",\n  \"viewerPsk\": \"" << json_escape(cfg.viewer_psk) << "\"";
  }
  if (!cfg.username.empty()) {
    out << ",\n  \"username\": \"" << json_escape(cfg.username) << "\"";
  }
  if (!cfg.viewer_instance_id.empty()) {
    out << ",\n  \"viewerInstanceId\": \"" << json_escape(cfg.viewer_instance_id) << "\"";
  }
  out << ",\n  \"rememberPassword\": " << (cfg.remember_password ? "true" : "false");
  if (cfg.remember_password && !cfg.password.empty()) {
    out << ",\n  \"password\": \"" << json_escape(cfg.password) << "\"";
  }
  out << "\n}\n";
  return true;
}

bool viewer_login(const std::string& gateway_url, const std::string& username,
                  const std::string& password, std::string* token, std::string* err) {
  if (!token) {
    return false;
  }
  const std::string body =
      std::string("{\"username\":\"") + json_escape(username) + "\",\"password\":\"" +
      json_escape(password) + "\"}";
  std::string resp;
  std::string e;
  if (!http_post_json(gateway_url, "/v1/viewer/login", body, &resp, &e)) {
    if (err) {
      *err = e;
    }
    return false;
  }
  *token = json_get_string(resp, "token");
  if (token->empty()) {
    if (err) {
      *err = "login response missing token";
    }
    return false;
  }
  return true;
}

std::string ensure_viewer_instance_id(DirectoryConfig* cfg) {
  if (!cfg) {
    return {};
  }
  if (!cfg->viewer_instance_id.empty()) {
    return cfg->viewer_instance_id;
  }
  DirectoryConfig disk = load_directory_config();
  if (!disk.viewer_instance_id.empty()) {
    cfg->viewer_instance_id = disk.viewer_instance_id;
    return cfg->viewer_instance_id;
  }
  GUID g{};
  char buf[64] = {};
  if (CoCreateGuid(&g) == S_OK) {
    std::snprintf(buf, sizeof(buf), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  static_cast<unsigned long>(g.Data1), g.Data2, g.Data3, g.Data4[0], g.Data4[1],
                  g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    for (char* p = buf; *p; ++p) {
      if (*p >= 'A' && *p <= 'F') {
        *p = static_cast<char>(*p - 'A' + 'a');
      }
    }
  } else {
    std::snprintf(buf, sizeof(buf), "viewer-%08lx%08lx",
                  static_cast<unsigned long>(GetTickCount()),
                  static_cast<unsigned long>(GetCurrentProcessId()));
  }
  cfg->viewer_instance_id = buf;
  // Merge into disk config so we don't wipe other fields if cfg is partial.
  if (disk.gateway_url.empty()) {
    disk.gateway_url = cfg->gateway_url;
  }
  if (disk.viewer_psk.empty()) {
    disk.viewer_psk = cfg->viewer_psk;
  }
  if (disk.username.empty()) {
    disk.username = cfg->username;
  }
  disk.viewer_instance_id = cfg->viewer_instance_id;
  if (cfg->remember_password) {
    disk.remember_password = true;
    if (!cfg->password.empty()) {
      disk.password = cfg->password;
    }
  }
  save_directory_config(disk);
  return cfg->viewer_instance_id;
}

bool prompt_directory_config(DirectoryConfig* inout) {
  if (!inout) {
    return false;
  }

  std::string pre_host = "127.0.0.1";
  std::string pre_port = "8743";
  if (!inout->gateway_url.empty()) {
    std::string h, p;
    if (split_gateway_url(inout->gateway_url, &h, &p)) {
      pre_host = h;
      pre_port = p;
    }
  }

  const wchar_t* kClass = L"RoadDeskViewerGatewayCfg";
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
  st.cfg = inout;
  st.psk_imported = !inout->viewer_psk.empty();
  st.dpi = dpi > 0 ? dpi : 96;
  st.font = make_font(rd::kFontBodyPt, FW_NORMAL);
  st.font_title = make_font(rd::kFontBodyPt, FW_SEMIBOLD);
  st.font_brand = make_font(rd::kFontBrandPt, FW_SEMIBOLD);
  st.font_login = make_font(rd::kFontLoginTitlePt, FW_SEMIBOLD);
  st.font_label = make_font(rd::kFontLabelPt, FW_NORMAL);
  st.font_mark = make_font(14, FW_BOLD);
  st.bg_brush = CreateSolidBrush(kBg);
  st.panel_brush = CreateSolidBrush(kPanel);
  st.edit_brush = CreateSolidBrush(kPanel);

  gdip::GdiplusStartupInput gdiplus_input;
  if (gdip::GdiplusStartup(&st.gdiplus_token, &gdiplus_input, nullptr) == gdip::Ok) {
    const std::wstring brand_path = find_brand_image();
    if (!brand_path.empty()) {
      st.brand_img = new gdip::Bitmap(brand_path.c_str());
      if (st.brand_img->GetLastStatus() != gdip::Ok) {
        delete st.brand_img;
        st.brand_img = nullptr;
      }
    }
  } else {
    st.gdiplus_token = 0;
  }

  g_dlg = &st;

  const int client_w = dip(&st, kClientW);
  const int client_h = dip(&st, st.psk_imported ? 500 : 540);
  RECT wr{0, 0, client_w, client_h};
  const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
  const DWORD ex_style = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
  AdjustWindowRectEx(&wr, style, FALSE, ex_style);
  const int win_w = wr.right - wr.left;
  const int win_h = wr.bottom - wr.top;

  HWND hwnd = CreateWindowExW(ex_style, kClass, L"Road Desk", style, 0, 0, win_w, win_h, nullptr,
                              nullptr, wc.hInstance, nullptr);
  if (!hwnd) {
    if (st.brand_img) {
      delete st.brand_img;
      st.brand_img = nullptr;
    }
    if (st.gdiplus_token) {
      gdip::GdiplusShutdown(st.gdiplus_token);
      st.gdiplus_token = 0;
    }
    g_dlg = nullptr;
    return false;
  }
  rd::rd_set_window_icons(hwnd, wc.hInstance);
  center_on_work_area(hwnd, win_w, win_h);

  const int left_w = dip(&st, kLeftW);
  const int pad_fx = dip(&st, kPadFormX);
  const int form_l = left_w + pad_fx;
  const int field_w = client_w - left_w - pad_fx * 2;
  const int edit_h = dip(&st, kEditH);
  const int label_h = dip(&st, kLabelH);
  const int port_w = dip(&st, kPortW);
  const int browse_w = dip(&st, kBrowseW);
  const int eye_w = dip(&st, kEyeW);
  const int icon_m = dip(&st, kIconPad);
  const int gap = dip(&st, 8);
  int y = dip(&st, kPadFormTop + 56);

  auto label = [&](const wchar_t* text, int yy, int x, int w) {
    HWND s = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, yy, w, label_h, hwnd, nullptr,
                           wc.hInstance, nullptr);
    apply_font(st.font_label, s);
  };

  label(L"网关主机", y, form_l, field_w - port_w - gap);
  label(L"端口", y, form_l + field_w - port_w, port_w);
  y += label_h + dip(&st, 6);
  st.host = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", pre_host.c_str(),
                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, form_l, y,
                            field_w - port_w - gap, edit_h, hwnd, nullptr, wc.hInstance, nullptr);
  st.port = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", pre_port.c_str(),
                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP,
                            form_l + field_w - port_w, y, port_w, edit_h, hwnd, nullptr,
                            wc.hInstance, nullptr);
  apply_font(st.font, st.host);
  apply_font(st.font, st.port);
  set_edit_icon_margin(st.host, icon_m);
  attach_edit_icon(st.host, LineIcon::Server);

  y += edit_h + dip(&st, kFieldGap);
  label(L"用户名", y, form_l, field_w);
  y += label_h + dip(&st, 6);
  st.user = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", utf8_to_wide(inout->username).c_str(),
                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, form_l, y, field_w,
                            edit_h, hwnd, nullptr, wc.hInstance, nullptr);
  apply_font(st.font, st.user);
  set_edit_icon_margin(st.user, icon_m);
  attach_edit_icon(st.user, LineIcon::User);

  y += edit_h + dip(&st, kFieldGap);
  label(L"密码", y, form_l, field_w);
  y += label_h + dip(&st, 6);
  st.pass = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, form_l,
                            y, field_w - eye_w - gap, edit_h, hwnd, nullptr, wc.hInstance, nullptr);
  st.eye = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                         form_l + field_w - eye_w, y, eye_w, edit_h, hwnd,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIDC_EYE)), wc.hInstance,
                         nullptr);
  apply_font(st.font, st.pass);
  set_edit_icon_margin(st.pass, icon_m);
  attach_edit_icon(st.pass, LineIcon::Lock);
  set_password_visible(&st, false);
  if (inout->remember_password && !inout->password.empty()) {
    SetWindowTextW(st.pass, utf8_to_wide(inout->password).c_str());
  }

  y += edit_h + dip(&st, 10);
  const int check_h = dip(&st, kCheckH);
  st.remember =
      CreateWindowW(L"BUTTON", L"记住密码", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                    form_l, y, field_w, check_h, hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIDC_REMEMBER)), wc.hInstance,
                    nullptr);
  apply_font(st.font_label, st.remember);
  SendMessageW(st.remember, BM_SETCHECK, inout->remember_password ? BST_CHECKED : BST_UNCHECKED, 0);

  y += check_h + dip(&st, 14);
  st.sep_y = y;
  y += dip(&st, 22);

  if (!st.psk_imported) {
    st.psk = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, form_l, y,
                             field_w - browse_w - gap, edit_h, hwnd, nullptr, wc.hInstance, nullptr);
    st.browse = CreateWindowW(L"BUTTON", L"浏览…", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                              form_l + field_w - browse_w, y, browse_w, edit_h, hwnd,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIDC_BROWSE)),
                              wc.hInstance, nullptr);
    apply_font(st.font, st.psk);
    set_edit_icon_margin(st.psk, icon_m);
    attach_edit_icon(st.psk, LineIcon::Key);
    SendMessageW(st.psk, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"粘贴或选择 viewer.psk"));
  }

  const int btn_h = dip(&st, kBtnH);
  const int btn_cancel_w = dip(&st, kBtnCancelW);
  const int btn_gap = dip(&st, 12);
  const int btn_y = client_h - dip(&st, 28) - btn_h;
  const int login_w = field_w - btn_cancel_w - btn_gap;
  st.cancel_btn =
      CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP, form_l,
                    btn_y, btn_cancel_w, btn_h, hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), wc.hInstance, nullptr);
  st.ok_btn = CreateWindowW(L"BUTTON", L"登录",
                            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_DEFPUSHBUTTON | WS_TABSTOP,
                            form_l + btn_cancel_w + btn_gap, btn_y, login_w, btn_h, hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), wc.hInstance,
                            nullptr);

  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);
  SetForegroundWindow(hwnd);
  if (inout->username.empty()) {
    SetFocus(st.user);
  } else if (!inout->remember_password || inout->password.empty()) {
    SetFocus(st.pass);
  } else {
    SetFocus(st.ok_btn);
  }

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

}  // namespace road_desk::viewer
