#include "auth.h"
#include "media_plane.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"RoadDeskViewerWindow";
constexpr UINT WM_MEDIA_RESIZE = WM_APP + 1;

road_desk::media::MediaClient* g_client = nullptr;
HWND g_hwnd = nullptr;
HHOOK g_kb_hook = nullptr;
int g_windowed_fb_w = 0;
int g_windowed_fb_h = 0;

bool fit_rect(int cw, int ch, int fb_w, int fb_h, RECT* out) {
  if (cw <= 0 || ch <= 0 || fb_w <= 0 || fb_h <= 0 || !out) {
    return false;
  }
  const double sx = static_cast<double>(cw) / fb_w;
  const double sy = static_cast<double>(ch) / fb_h;
  const double s = (sx < sy) ? sx : sy;
  const int dw = static_cast<int>(fb_w * s + 0.5);
  const int dh = static_cast<int>(fb_h * s + 0.5);
  out->left = (cw - dw) / 2;
  out->top = (ch - dh) / 2;
  out->right = out->left + dw;
  out->bottom = out->top + dh;
  return true;
}

void apply_resize_to_fb(HWND hwnd, int fb_w, int fb_h) {
  if (fb_w <= 0 || fb_h <= 0) {
    return;
  }
  if (fb_w == g_windowed_fb_w && fb_h == g_windowed_fb_h) {
    return;
  }
  g_windowed_fb_w = fb_w;
  g_windowed_fb_h = fb_h;
  RECT want{0, 0, fb_w, fb_h};
  const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
  const DWORD ex = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
  AdjustWindowRectEx(&want, style, FALSE, ex);
  SetWindowPos(hwnd, nullptr, 0, 0, want.right - want.left, want.bottom - want.top,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  wchar_t title[128];
  _snwprintf_s(title, _TRUNCATE, L"Road Desk Viewer (%dx%d)", fb_w, fb_h);
  SetWindowTextW(hwnd, title);
}

// Persistent back-buffer — recreating a full-window bitmap every paint kills drag FPS.
HDC g_back_dc = nullptr;
HBITMAP g_back_bmp = nullptr;
HGDIOBJ g_back_old = nullptr;
int g_back_w = 0;
int g_back_h = 0;

void release_backbuffer() {
  if (g_back_dc && g_back_old) {
    SelectObject(g_back_dc, g_back_old);
    g_back_old = nullptr;
  }
  if (g_back_bmp) {
    DeleteObject(g_back_bmp);
    g_back_bmp = nullptr;
  }
  if (g_back_dc) {
    DeleteDC(g_back_dc);
    g_back_dc = nullptr;
  }
  g_back_w = 0;
  g_back_h = 0;
}

bool ensure_backbuffer(HDC hdc, int cw, int ch) {
  if (cw <= 0 || ch <= 0) {
    return false;
  }
  if (g_back_dc && g_back_bmp && g_back_w == cw && g_back_h == ch) {
    return true;
  }
  release_backbuffer();
  g_back_dc = CreateCompatibleDC(hdc);
  g_back_bmp = CreateCompatibleBitmap(hdc, cw, ch);
  if (!g_back_dc || !g_back_bmp) {
    release_backbuffer();
    return false;
  }
  g_back_old = SelectObject(g_back_dc, g_back_bmp);
  g_back_w = cw;
  g_back_h = ch;
  return true;
}

void paint(HWND hwnd) {
  PAINTSTRUCT ps{};
  HDC hdc = BeginPaint(hwnd, &ps);

  std::vector<uint8_t> bgra;
  int w = 0;
  int h = 0;
  if (g_client) {
    g_client->copy_frame_bgra(bgra, w, h);
  }

  RECT rc{};
  GetClientRect(hwnd, &rc);
  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;
  if (cw <= 0 || ch <= 0 || !ensure_backbuffer(hdc, cw, ch)) {
    EndPaint(hwnd, &ps);
    return;
  }

  FillRect(g_back_dc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

  RECT dest{};
  if (w > 0 && h > 0 && fit_rect(cw, ch, w, h, &dest) &&
      bgra.size() >= static_cast<size_t>(w) * h * 4) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    const int dw = dest.right - dest.left;
    const int dh = dest.bottom - dest.top;
    SetStretchBltMode(g_back_dc, COLORONCOLOR);
    if (dw == w && dh == h) {
      SetDIBitsToDevice(g_back_dc, dest.left, dest.top, w, h, 0, 0, 0, h, bgra.data(), &bmi,
                        DIB_RGB_COLORS);
    } else {
      StretchDIBits(g_back_dc, dest.left, dest.top, dw, dh, 0, 0, w, h, bgra.data(), &bmi,
                    DIB_RGB_COLORS, SRCCOPY);
    }
  } else {
    const wchar_t* msg = L"Connecting / waiting for framebuffer…";
    SetBkMode(g_back_dc, TRANSPARENT);
    SetTextColor(g_back_dc, RGB(220, 220, 220));
    DrawTextW(g_back_dc, msg, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  BitBlt(hdc, 0, 0, cw, ch, g_back_dc, 0, 0, SRCCOPY);
  EndPaint(hwnd, &ps);
}

int map_mouse_x(HWND hwnd, LPARAM lparam, int fb_w, int fb_h) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  RECT dest{};
  if (!fit_rect(rc.right - rc.left, rc.bottom - rc.top, fb_w, fb_h, &dest)) {
    return 0;
  }
  const int dw = dest.right - dest.left;
  if (dw <= 0) {
    return 0;
  }
  int x = ((GET_X_LPARAM(lparam) - dest.left) * fb_w) / dw;
  if (x < 0) {
    x = 0;
  }
  if (x >= fb_w) {
    x = fb_w - 1;
  }
  return x;
}

int map_mouse_y(HWND hwnd, LPARAM lparam, int fb_w, int fb_h) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  RECT dest{};
  if (!fit_rect(rc.right - rc.left, rc.bottom - rc.top, fb_w, fb_h, &dest)) {
    return 0;
  }
  const int dh = dest.bottom - dest.top;
  if (dh <= 0) {
    return 0;
  }
  int y = ((GET_Y_LPARAM(lparam) - dest.top) * fb_h) / dh;
  if (y < 0) {
    y = 0;
  }
  if (y >= fb_h) {
    y = fb_h - 1;
  }
  return y;
}

LRESULT CALLBACK low_level_keyboard(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION && g_hwnd && g_client && g_client->connected() &&
      GetForegroundWindow() == g_hwnd) {
    const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
    if (!(info->flags & LLKHF_INJECTED)) {
      const bool down = (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN);
      if (g_client->send_vk(info->vkCode, down)) {
        return 1;
      }
    }
  }
  return CallNextHookEx(g_kb_hook, code, wparam, lparam);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_MEDIA_RESIZE:
      apply_resize_to_fb(hwnd, static_cast<int>(wparam), static_cast<int>(lparam));
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_PAINT:
      paint(hwnd);
      return 0;
    case WM_DESTROY:
      release_backbuffer();
      PostQuitMessage(0);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSEMOVE: {
      if (!g_client || !g_client->connected()) {
        break;
      }
      int mask = 0;
      if (wparam & MK_LBUTTON) {
        mask |= 1;
      }
      if (wparam & MK_MBUTTON) {
        mask |= 2;
      }
      if (wparam & MK_RBUTTON) {
        mask |= 4;
      }
      std::vector<uint8_t> unused;
      int fb_w = 0;
      int fb_h = 0;
      g_client->copy_frame_bgra(unused, fb_w, fb_h);
      if (fb_w > 0 && fb_h > 0) {
        g_client->send_pointer(mask, map_mouse_x(hwnd, lparam, fb_w, fb_h),
                               map_mouse_y(hwnd, lparam, fb_w, fb_h));
      }
      return 0;
    }
    case WM_KILLFOCUS:
    case WM_ACTIVATE:
      if (msg == WM_KILLFOCUS ||
          (msg == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE)) {
        if (g_client) {
          g_client->release_modifiers();
        }
      }
      break;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
      if (!g_kb_hook && g_client) {
        g_client->send_vk(static_cast<unsigned>(wparam),
                          msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

void write_viewer_boot(const char* text) {
  char path[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return;
  }
  char* slash = path;
  for (char* p = path; *p; ++p) {
    if (*p == '\\' || *p == '/') {
      slash = p + 1;
    }
  }
  constexpr char name[] = "viewer.boot";
  if (static_cast<size_t>(path + MAX_PATH - slash) <= sizeof(name)) {
    return;
  }
  memcpy(slash, name, sizeof(name));
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") == 0 && f) {
    fputs(text, f);
    fclose(f);
  }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmd_line, int show_cmd) {
  write_viewer_boot("main\n");

  if (AllocConsole()) {
    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
  }
  write_viewer_boot("console_ok\n");

  std::string host = "127.0.0.1:5900";
  std::string password = "road-desk";
  std::string tls_fingerprint;
  if (cmd_line && cmd_line[0] != L'\0') {
    char narrow[512] = {};
    // CP_ACP is safer on Win7 consoles than forcing UTF-8 for ASCII host:port.
    WideCharToMultiByte(CP_ACP, 0, cmd_line, -1, narrow, sizeof(narrow), nullptr, nullptr);
    char* next = nullptr;
    char* tok = strtok_s(narrow, " \t", &next);
    if (tok) {
      host = tok;
    }
    tok = strtok_s(nullptr, " \t", &next);
    if (tok) {
      password = tok;
    }
    tok = strtok_s(nullptr, " \t", &next);
    if (tok) {
      tls_fingerprint = tok;
    }
  }
  {
    // If ROAD_DESK_PSK is set (even empty), it overrides argv — empty => fail-closed.
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "ROAD_DESK_PSK") == 0 && env) {
      password = env;
    }
    free(env);
  }
  {
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "ROAD_DESK_TLS_FINGERPRINT") == 0 && env && env[0]) {
      tls_fingerprint = env;
    }
    free(env);
  }
  if (!road_desk::session::authenticate_psk(password, password)) {
    write_viewer_boot("psk_fail\n");
    std::fprintf(stderr, "PSK required — set ROAD_DESK_PSK or pass password; refusing start\n");
    return 1;
  }
  write_viewer_boot("args_ok\n");

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&wc)) {
    write_viewer_boot("register_fail\n");
    return 1;
  }

  g_hwnd = CreateWindowExW(0, kWindowClass, L"Road Desk Viewer", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768, nullptr, nullptr, instance,
                           nullptr);
  if (!g_hwnd) {
    write_viewer_boot("create_fail\n");
    return 1;
  }
  ShowWindow(g_hwnd, show_cmd);
  write_viewer_boot("window_ok\n");

  road_desk::media::MediaClient client;
  g_client = &client;
  road_desk::media::MediaClientConfig cfg;
  cfg.host_port = host;
  cfg.password = password;
  cfg.notify_hwnd = g_hwnd;
  cfg.resize_msg = WM_MEDIA_RESIZE;
  {
    char* env = nullptr;
    size_t len = 0;
    bool allow_plain = false;
    if (_dupenv_s(&env, &len, "ROAD_DESK_ALLOW_PLAINTEXT") == 0 && env &&
        (env[0] == '1' || env[0] == 'y' || env[0] == 'Y')) {
      allow_plain = true;
    }
    free(env);
    cfg.require_tls = !allow_plain;
  }
  {
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "ROAD_DESK_TLS_INSECURE") == 0 && env &&
        (env[0] == '1' || env[0] == 'y' || env[0] == 'Y')) {
      cfg.tls_insecure = true;
    }
    free(env);
  }
  cfg.tls_fingerprint_sha256 = tls_fingerprint;
  if (cfg.require_tls && !cfg.tls_insecure && cfg.tls_fingerprint_sha256.empty()) {
    write_viewer_boot("tls_fp_missing\n");
    std::fprintf(stderr,
                 "TLS fingerprint required (ROAD_DESK_TLS_FINGERPRINT or 3rd arg), "
                 "or set ROAD_DESK_TLS_INSECURE=1 for debug\n");
    g_client = nullptr;
    return 1;
  }
  write_viewer_boot("before_start\n");
  if (!client.start(cfg)) {
    write_viewer_boot("start_fail\n");
    g_client = nullptr;
    return 1;
  }
  write_viewer_boot("started\n");

  // Install hook after client start (spike order was reverse; hook is optional).
  g_kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, low_level_keyboard, instance, 0);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  if (g_kb_hook) {
    UnhookWindowsHookEx(g_kb_hook);
    g_kb_hook = nullptr;
  }
  g_client = nullptr;
  client.stop();
  write_viewer_boot("stopped\n");
  return 0;
}
