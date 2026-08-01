#include "auth.h"
#include "media_log.h"
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
bool g_tracking_leave = false;

bool fit_rect(int cw, int ch, int fb_w, int fb_h, RECT* out) {
  if (cw <= 0 || ch <= 0 || fb_w <= 0 || fb_h <= 0 || !out) {
    return false;
  }
  const double sx = static_cast<double>(cw) / fb_w;
  const double sy = static_cast<double>(ch) / fb_h;
  const double s = (sx < sy) ? sx : sy;
  int dw = 0;
  int dh = 0;
  if (s >= 1.0) {
    // Integer zoom only when enlarging — fractional upscale turns ClearType text to mush.
    const int si = static_cast<int>(s);
    dw = fb_w * si;
    dh = fb_h * si;
  } else {
    dw = static_cast<int>(fb_w * s + 0.5);
    dh = static_cast<int>(fb_h * s + 0.5);
    if (dw < 1) {
      dw = 1;
    }
    if (dh < 1) {
      dh = 1;
    }
  }
  out->left = (cw - dw) / 2;
  out->top = (ch - dh) / 2;
  out->right = out->left + dw;
  out->bottom = out->top + dh;
  return true;
}

// GDI StretchDIBits often soft-filters even with COLORONCOLOR; do nearest-neighbor ourselves.
void scale_nearest_bgra(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
  if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
    return;
  }
  if (dw == sw && dh == sh) {
    memcpy(dst, src, static_cast<size_t>(sw) * sh * 4);
    return;
  }
  // Integer upscale: replicate pixels (fast + perfectly sharp).
  if (dw % sw == 0 && dh % sh == 0) {
    const int mx = dw / sw;
    const int my = dh / sh;
    for (int y = 0; y < sh; ++y) {
      const uint8_t* srow = src + static_cast<size_t>(y) * sw * 4;
      for (int row = 0; row < my; ++row) {
        uint8_t* drow = dst + static_cast<size_t>(y * my + row) * dw * 4;
        if (mx == 1) {
          memcpy(drow, srow, static_cast<size_t>(sw) * 4);
        } else {
          for (int x = 0; x < sw; ++x) {
            const uint8_t* p = srow + static_cast<size_t>(x) * 4;
            for (int col = 0; col < mx; ++col) {
              uint8_t* q = drow + static_cast<size_t>(x * mx + col) * 4;
              q[0] = p[0];
              q[1] = p[1];
              q[2] = p[2];
              q[3] = p[3];
            }
          }
        }
      }
    }
    return;
  }
  for (int y = 0; y < dh; ++y) {
    const int sy = y * sh / dh;
    const uint8_t* srow = src + static_cast<size_t>(sy) * sw * 4;
    uint8_t* drow = dst + static_cast<size_t>(y) * dw * 4;
    for (int x = 0; x < dw; ++x) {
      const int sx = x * sw / dw;
      const uint8_t* p = srow + static_cast<size_t>(sx) * 4;
      uint8_t* q = drow + static_cast<size_t>(x) * 4;
      q[0] = p[0];
      q[1] = p[1];
      q[2] = p[2];
      q[3] = p[3];
    }
  }
}

std::vector<uint8_t> g_scale_bgra;

void track_mouse_leave(HWND hwnd) {
  if (g_tracking_leave) {
    return;
  }
  TRACKMOUSEEVENT tme{};
  tme.cbSize = sizeof(tme);
  tme.dwFlags = TME_LEAVE;
  tme.hwndTrack = hwnd;
  if (TrackMouseEvent(&tme)) {
    g_tracking_leave = true;
  }
}

bool client_point_in_letterbox(HWND hwnd, LPARAM lparam, int fb_w, int fb_h) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  RECT dest{};
  if (!fit_rect(rc.right - rc.left, rc.bottom - rc.top, fb_w, fb_h, &dest)) {
    return false;
  }
  const int cx = GET_X_LPARAM(lparam);
  const int cy = GET_Y_LPARAM(lparam);
  return cx >= dest.left && cx < dest.right && cy >= dest.top && cy < dest.bottom;
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
    const int dw = dest.right - dest.left;
    const int dh = dest.bottom - dest.top;
    const uint8_t* bits = bgra.data();
    int bits_w = w;
    int bits_h = h;
    if (dw != w || dh != h) {
      g_scale_bgra.resize(static_cast<size_t>(dw) * dh * 4);
      scale_nearest_bgra(bgra.data(), w, h, g_scale_bgra.data(), dw, dh);
      bits = g_scale_bgra.data();
      bits_w = dw;
      bits_h = dh;
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bits_w;
    bmi.bmiHeader.biHeight = -bits_h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    // Always 1:1 blit after optional CPU nearest-neighbor scale (no GDI stretch filter).
    SetDIBitsToDevice(g_back_dc, dest.left, dest.top, bits_w, bits_h, 0, 0, 0, bits_h, bits,
                      &bmi, DIB_RGB_COLORS);
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
    case WM_SETCURSOR:
      // Suppress OS arrow — software cursor is painted into the framebuffer.
      if (LOWORD(lparam) == HTCLIENT) {
        SetCursor(nullptr);
        return TRUE;
      }
      break;
    case WM_MOUSELEAVE:
      g_tracking_leave = false;
      if (g_client) {
        g_client->set_software_cursor_enabled(false);
      }
      return 0;
    case WM_CLIPBOARDUPDATE:
      if (g_client) {
        g_client->notify_clipboard_changed();
      }
      return 0;
    case WM_DESTROY:
      RemoveClipboardFormatListener(hwnd);
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
      if (msg == WM_MOUSEMOVE) {
        track_mouse_leave(hwnd);
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
        if (!client_point_in_letterbox(hwnd, lparam, fb_w, fb_h)) {
          g_client->set_software_cursor_enabled(false);
          return 0;
        }
        g_client->set_software_cursor_enabled(true);
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
          g_client->set_software_cursor_enabled(false);
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

void viewer_boot(const char* step) {
  if (!road_desk::media::media_log_is_open()) {
    road_desk::media::media_log_open("viewer.log");
  }
  road_desk::media::media_logf("viewer", "boot %s", step);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmd_line, int show_cmd) {
  viewer_boot("main");
  // System-DPI aware: without this, Win8+/125%–150% bitmap-stretches the whole
  // window and the remote desktop looks soft/blurry (pitfall L1).
  SetProcessDPIAware();

  if (AllocConsole()) {
    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
  }
  viewer_boot("console_ok");

  std::string host = "127.0.0.1:38471";
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
    viewer_boot("psk_fail");
    std::fprintf(stderr, "PSK required — set ROAD_DESK_PSK or pass password; refusing start\n");
    return 1;
  }
  viewer_boot("args_ok");

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  // Soft cursor is composited into copy_frame_bgra (mux client).
  wc.hCursor = nullptr;
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&wc)) {
    viewer_boot("register_fail");
    return 1;
  }

  g_hwnd = CreateWindowExW(0, kWindowClass, L"Road Desk Viewer", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768, nullptr, nullptr, instance,
                           nullptr);
  if (!g_hwnd) {
    viewer_boot("create_fail");
    return 1;
  }
  if (!AddClipboardFormatListener(g_hwnd)) {
    viewer_boot("clip_listener_fail");
    std::fprintf(stderr, "AddClipboardFormatListener failed (%lu)\n", GetLastError());
    return 1;
  }
  ShowWindow(g_hwnd, show_cmd);
  viewer_boot("window_ok");

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
    viewer_boot("tls_fp_missing");
    std::fprintf(stderr,
                 "TLS fingerprint required (ROAD_DESK_TLS_FINGERPRINT or 3rd arg), "
                 "or set ROAD_DESK_TLS_INSECURE=1 for debug\n");
    g_client = nullptr;
    return 1;
  }
  viewer_boot("before_start");
  if (!client.start(cfg)) {
    viewer_boot("start_fail");
    g_client = nullptr;
    return 1;
  }
  viewer_boot("started");

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
  viewer_boot("stopped");
  road_desk::media::media_log_close();
  return 0;
}
