// Media-plane spike Viewer: LibVNCClient + minimal Win32 paint surface.
// GPL candidate for internal validation only.

#include <rfb/rfbclient.h>
#include <rfb/keysym.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kClassName[] = L"RoadDeskSpikeViewer";
constexpr char kPassword[] = "spike";
constexpr UINT WM_SPIKE_RESIZE_TO_FB = WM_APP + 1;

HWND g_hwnd = nullptr;
rfbClient* g_client = nullptr;
std::mutex g_fb_mu;
// Persistent BGRA buffer for GDI (updated incrementally on dirty rects).
std::vector<unsigned char> g_bgra;
int g_fb_w = 0;
int g_fb_h = 0;
int g_windowed_fb_w = 0;
int g_windowed_fb_h = 0;
int g_rshift = 0;
int g_gshift = 8;
int g_bshift = 16;
bool g_running = true;

void convert_rect_to_bgra(const uint8_t* fb, int fb_w, int fb_h, int x, int y, int w,
                          int h, int rshift, int gshift, int bshift) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > fb_w) {
    w = fb_w - x;
  }
  if (y + h > fb_h) {
    h = fb_h - y;
  }
  if (w <= 0 || h <= 0 || g_bgra.size() < static_cast<size_t>(fb_w) * fb_h * 4) {
    return;
  }
  const auto* src = reinterpret_cast<const uint32_t*>(fb);
  for (int row = 0; row < h; ++row) {
    const int yy = y + row;
    for (int col = 0; col < w; ++col) {
      const int xx = x + col;
      const uint32_t p = src[yy * fb_w + xx];
      const size_t di = (static_cast<size_t>(yy) * fb_w + xx) * 4;
      g_bgra[di + 0] = static_cast<unsigned char>((p >> bshift) & 0xff);
      g_bgra[di + 1] = static_cast<unsigned char>((p >> gshift) & 0xff);
      g_bgra[di + 2] = static_cast<unsigned char>((p >> rshift) & 0xff);
      g_bgra[di + 3] = 0;
    }
  }
}

void request_resize_to_fb(int w, int h) {
  if (!g_hwnd || w <= 0 || h <= 0) {
    return;
  }
  if (w == g_windowed_fb_w && h == g_windowed_fb_h) {
    return;
  }
  g_windowed_fb_w = w;
  g_windowed_fb_h = h;
  PostMessageW(g_hwnd, WM_SPIKE_RESIZE_TO_FB, static_cast<WPARAM>(w), static_cast<LPARAM>(h));
}

void apply_resize_to_fb(HWND hwnd, int fb_w, int fb_h) {
  // Initial open at remote size; user may scale window later (like TightVNC scale-to-window).
  RECT want{0, 0, fb_w, fb_h};
  const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
  const DWORD ex = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
  AdjustWindowRectEx(&want, style, FALSE, ex);
  SetWindowPos(hwnd, nullptr, 0, 0, want.right - want.left, want.bottom - want.top,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

  wchar_t title[128];
  _snwprintf_s(title, _TRUNCATE, L"Road Desk Spike Viewer (%dx%d)", fb_w, fb_h);
  SetWindowTextW(hwnd, title);
}

char* get_password(rfbClient* /*client*/) {
  return _strdup(kPassword);
}

rfbBool on_malloc_framebuffer(rfbClient* client) {
  free(client->frameBuffer);
  client->frameBuffer = nullptr;
  const size_t bytes =
      static_cast<size_t>(client->width) * client->height * (client->format.bitsPerPixel / 8);
  client->frameBuffer = static_cast<uint8_t*>(malloc(bytes));
  request_resize_to_fb(client->width, client->height);
  return client->frameBuffer != nullptr;
}

void on_update(rfbClient* client, int x, int y, int w, int h) {
  {
    std::lock_guard<std::mutex> lock(g_fb_mu);
    g_fb_w = client->width;
    g_fb_h = client->height;
    const rfbPixelFormat& pf = client->format;
    g_rshift = pf.redShift;
    g_gshift = pf.greenShift;
    g_bshift = pf.blueShift;
    const size_t bytes = static_cast<size_t>(g_fb_w) * g_fb_h * 4;
    if (g_bgra.size() != bytes) {
      // Build into a side buffer so paint never sees a cleared/black frame mid-resize.
      std::vector<unsigned char> next(bytes, 0);
      g_bgra.swap(next);
      if (client->frameBuffer) {
        convert_rect_to_bgra(client->frameBuffer, g_fb_w, g_fb_h, 0, 0, g_fb_w, g_fb_h,
                             g_rshift, g_gshift, g_bshift);
      }
    } else if (client->frameBuffer) {
      // Clamp bogus rects from some encodings / ExtDesktopSize races.
      if (w <= 0 || h <= 0 || x >= g_fb_w || y >= g_fb_h) {
        convert_rect_to_bgra(client->frameBuffer, g_fb_w, g_fb_h, 0, 0, g_fb_w, g_fb_h,
                             g_rshift, g_gshift, g_bshift);
      } else {
        convert_rect_to_bgra(client->frameBuffer, g_fb_w, g_fb_h, x, y, w, h, g_rshift,
                             g_gshift, g_bshift);
      }
    }
  }
  request_resize_to_fb(client->width, client->height);
  if (g_hwnd) {
    // Full invalidate: partial blits tore with top-down DIBs under load.
    InvalidateRect(g_hwnd, nullptr, FALSE);
  }
}

// Letterbox rect: fit FB into client while preserving aspect ratio (like VNC Viewer).
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

void paint(HWND hwnd) {
  PAINTSTRUCT ps{};
  HDC hdc = BeginPaint(hwnd, &ps);

  std::vector<unsigned char> bgra;
  int w = 0;
  int h = 0;
  {
    std::lock_guard<std::mutex> lock(g_fb_mu);
    w = g_fb_w;
    h = g_fb_h;
    bgra = g_bgra;
  }

  RECT rc{};
  GetClientRect(hwnd, &rc);
  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;
  if (cw <= 0 || ch <= 0) {
    EndPaint(hwnd, &ps);
    return;
  }

  // Double-buffer: avoid visible black clear between erase and slow HALFTONE stretch (闪屏).
  HDC mem = CreateCompatibleDC(hdc);
  HBITMAP bmp = CreateCompatibleBitmap(hdc, cw, ch);
  HGDIOBJ old = SelectObject(mem, bmp);
  FillRect(mem, &rc, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

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
    SetStretchBltMode(mem, HALFTONE);
    SetBrushOrgEx(mem, 0, 0, nullptr);
    StretchDIBits(mem, dest.left, dest.top, dest.right - dest.left, dest.bottom - dest.top, 0, 0,
                  w, h, bgra.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
  } else {
    const wchar_t* msg = L"Connecting / waiting for framebuffer…";
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(220, 220, 220));
    DrawTextW(mem, msg, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  EndPaint(hwnd, &ps);
}

int map_mouse_x(HWND hwnd, LPARAM lparam, int fb_w) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;
  int fb_h = 0;
  {
    std::lock_guard<std::mutex> lock(g_fb_mu);
    fb_h = g_fb_h;
  }
  RECT dest{};
  if (!fit_rect(cw, ch, fb_w, fb_h, &dest)) {
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

int map_mouse_y(HWND hwnd, LPARAM lparam, int fb_h) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;
  int fb_w = 0;
  {
    std::lock_guard<std::mutex> lock(g_fb_mu);
    fb_w = g_fb_w;
  }
  RECT dest{};
  if (!fit_rect(cw, ch, fb_w, fb_h, &dest)) {
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

rfbKeySym vk_to_keysym(WPARAM vk, bool /*shift*/) {
  switch (vk) {
    case VK_BACK:
      return XK_BackSpace;
    case VK_TAB:
      return XK_Tab;
    case VK_RETURN:
      return XK_Return;
    case VK_ESCAPE:
      return XK_Escape;
    case VK_DELETE:
      return XK_Delete;
    case VK_LEFT:
      return XK_Left;
    case VK_UP:
      return XK_Up;
    case VK_RIGHT:
      return XK_Right;
    case VK_DOWN:
      return XK_Down;
    case VK_HOME:
      return XK_Home;
    case VK_END:
      return XK_End;
    case VK_PRIOR:
      return XK_Page_Up;
    case VK_NEXT:
      return XK_Page_Down;
    case VK_INSERT:
      return XK_Insert;
    case VK_LSHIFT:
    case VK_SHIFT:
      return XK_Shift_L;
    case VK_RSHIFT:
      return XK_Shift_R;
    case VK_LCONTROL:
    case VK_CONTROL:
      return XK_Control_L;
    case VK_RCONTROL:
      return XK_Control_R;
    case VK_LMENU:
    case VK_MENU:
      return XK_Alt_L;
    case VK_RMENU:
      return XK_Alt_R;
    case VK_LWIN:
      return XK_Super_L;
    case VK_RWIN:
      return XK_Super_R;
    case VK_F1:
      return XK_F1;
    case VK_F2:
      return XK_F2;
    case VK_F3:
      return XK_F3;
    case VK_F4:
      return XK_F4;
    case VK_F5:
      return XK_F5;
    case VK_F6:
      return XK_F6;
    case VK_F7:
      return XK_F7;
    case VK_F8:
      return XK_F8;
    case VK_F9:
      return XK_F9;
    case VK_F10:
      return XK_F10;
    case VK_F11:
      return XK_F11;
    case VK_F12:
      return XK_F12;
    case VK_SPACE:
      return XK_space;
    default:
      break;
  }
  if (vk >= 0x30 && vk <= 0x39) {
    return static_cast<rfbKeySym>(vk);
  }
  if (vk >= 0x41 && vk <= 0x5a) {
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
    const bool upper = shift ^ caps;
    return static_cast<rfbKeySym>(upper ? vk : (vk + 32));
  }
  return 0;
}

HHOOK g_kb_hook = nullptr;

// Classic VNC viewers use a low-level hook so Alt+Tab / Win stay remote.
LRESULT CALLBACK low_level_keyboard(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION && g_hwnd && g_client && GetForegroundWindow() == g_hwnd) {
    const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
    if (!(info->flags & LLKHF_INJECTED)) {
      const bool down = (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN);
      const rfbKeySym ks = vk_to_keysym(info->vkCode, false);
      if (ks != 0) {
        SendKeyEvent(g_client, ks, down ? TRUE : FALSE);
        return 1;  // swallow — otherwise shell steals Alt+Tab locally
      }
    }
  }
  return CallNextHookEx(g_kb_hook, code, wparam, lparam);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_SPIKE_RESIZE_TO_FB:
      apply_resize_to_fb(hwnd, static_cast<int>(wparam), static_cast<int>(lparam));
      return 0;
    case WM_ERASEBKGND:
      // Prevent GDI erase flash (white/black) before WM_PAINT.
      return 1;
    case WM_SIZE:
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_PAINT:
      paint(hwnd);
      return 0;
    case WM_DESTROY:
      g_running = false;
      PostQuitMessage(0);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSEMOVE: {
      if (!g_client) {
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
      int fb_w = 0;
      int fb_h = 0;
      {
        std::lock_guard<std::mutex> lock(g_fb_mu);
        fb_w = g_fb_w;
        fb_h = g_fb_h;
      }
      SendPointerEvent(g_client, map_mouse_x(hwnd, lparam, fb_w),
                       map_mouse_y(hwnd, lparam, fb_h), mask);
      return 0;
    }
    case WM_KILLFOCUS:
    case WM_ACTIVATE:
      // Switching to another window (e.g. classic VNC) often drops Alt/Ctrl KEYUP.
      if (msg == WM_KILLFOCUS ||
          (msg == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE)) {
        if (g_client) {
          SendKeyEvent(g_client, XK_Shift_L, FALSE);
          SendKeyEvent(g_client, XK_Control_L, FALSE);
          SendKeyEvent(g_client, XK_Alt_L, FALSE);
          SendKeyEvent(g_client, XK_Super_L, FALSE);
        }
      }
      break;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
      // Normal keys are forwarded by WH_KEYBOARD_LL while focused (avoids Alt+Tab
      // being eaten by the local shell). Keep this as a fallback if the hook misses.
      if (!g_kb_hook && g_client) {
        const rfbKeySym ks = vk_to_keysym(wparam, false);
        if (ks != 0) {
          SendKeyEvent(g_client, ks, msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        }
        return 0;
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

DWORD WINAPI client_thread(LPVOID param) {
  auto* host = static_cast<char*>(param);
  rfbClient* client = rfbGetClient(8, 3, 4);
  if (!client) {
    return 1;
  }
  client->MallocFrameBuffer = on_malloc_framebuffer;
  client->GotFrameBufferUpdate = on_update;
  client->GetPassword = get_password;
  // Disable desktop-size extensions — they raced with our capture path and caused 花屏.
  client->canHandleNewFBSize = FALSE;
  // Prefer hextile (stable). Avoid copyrect until we implement move detection.
  client->appData.encodingsString = const_cast<char*>("hextile ultra raw");
  client->appData.compressLevel = 1;
  client->appData.qualityLevel = 6;

  char* argv_storage[] = {const_cast<char*>("spike_viewer"), host, nullptr};
  int argc = 2;
  char** argv = argv_storage;
  if (!rfbInitClient(client, &argc, argv)) {
    std::fprintf(stderr, "spike_viewer: connect failed\n");
    free(host);
    return 1;
  }

  g_client = client;
  std::printf("spike_viewer connected to %s\n", host);
  std::fflush(stdout);
  free(host);

  while (g_running) {
    const int n = WaitForMessage(client, 5000);
    if (n < 0) {
      break;
    }
    if (n) {
      if (!HandleRFBServerMessage(client)) {
        break;
      }
    }
  }

  g_client = nullptr;
  rfbClientCleanup(client);
  if (g_hwnd) {
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
  }
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmd_line, int show_cmd) {
  // Intentionally DPI-unaware: classic WinVNC viewers size in logical pixels the same way.
  AllocConsole();
  FILE* dummy = nullptr;
  freopen_s(&dummy, "CONOUT$", "w", stdout);
  freopen_s(&dummy, "CONOUT$", "w", stderr);

  std::string host = "127.0.0.1:5900";
  if (cmd_line && cmd_line[0] != L'\0') {
    char narrow[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, cmd_line, -1, narrow, sizeof(narrow), nullptr, nullptr);
    host = narrow;
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // we paint fully; avoids erase flicker
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);

  g_hwnd = CreateWindowExW(0, kClassName, L"Road Desk Spike Viewer", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768, nullptr, nullptr, instance,
                           nullptr);
  ShowWindow(g_hwnd, show_cmd);

  g_kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, low_level_keyboard, instance, 0);
  if (!g_kb_hook) {
    std::fprintf(stderr, "spike_viewer: WH_KEYBOARD_LL failed (%lu); Alt+Tab stays local\n",
                 GetLastError());
  }

  char* host_copy = _strdup(host.c_str());
  HANDLE th = CreateThread(nullptr, 0, client_thread, host_copy, 0, nullptr);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  if (g_kb_hook) {
    UnhookWindowsHookEx(g_kb_hook);
    g_kb_hook = nullptr;
  }
  g_running = false;
  if (th) {
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
  }
  return 0;
}
