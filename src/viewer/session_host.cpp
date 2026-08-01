#include "session_host.h"

#include <windowsx.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace road_desk::viewer {
namespace {

constexpr wchar_t kSessionClass[] = L"RoadDeskSessionHost";

SessionHost* g_kb_target = nullptr;
HHOOK g_kb_hook = nullptr;

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

void scale_nearest_bgra(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
  if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
    return;
  }
  if (dw == sw && dh == sh) {
    memcpy(dst, src, static_cast<size_t>(sw) * sh * 4);
    return;
  }
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
  if (code == HC_ACTION && g_kb_target && g_kb_target->wants_keyboard() &&
      g_kb_target->connected()) {
    const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
    if (!(info->flags & LLKHF_INJECTED)) {
      const bool down = (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN);
      if (g_kb_target->send_vk(info->vkCode, down)) {
        return 1;
      }
    }
  }
  return CallNextHookEx(g_kb_hook, code, wparam, lparam);
}

}  // namespace

LRESULT CALLBACK SessionWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  SessionHost* self = SessionHost::from_hwnd(hwnd);
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<SessionHost*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    if (self) {
      self->hwnd_ = hwnd;
    }
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  switch (msg) {
    case WM_MEDIA_RESIZE:
      // Embedded: do not resize console; floating: optional title update only.
      if (self->floating_) {
        wchar_t title[160];
        _snwprintf_s(title, _TRUNCATE, L"%s (%dx%d)", self->title_.c_str(),
                     static_cast<int>(wparam), static_cast<int>(lparam));
        SetWindowTextW(hwnd, title);
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_PAINT:
      self->paint();
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT) {
        SetCursor(nullptr);
        return TRUE;
      }
      break;
    case WM_MOUSELEAVE:
      self->tracking_leave_ = false;
      self->client_.set_software_cursor_enabled(false);
      return 0;
    case WM_CLIPBOARDUPDATE:
      self->client_.notify_clipboard_changed();
      return 0;
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      self->on_destroy();
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSEMOVE: {
      if (!self->client_.connected()) {
        break;
      }
      if (msg == WM_MOUSEMOVE) {
        if (!self->tracking_leave_) {
          TRACKMOUSEEVENT tme{};
          tme.cbSize = sizeof(tme);
          tme.dwFlags = TME_LEAVE;
          tme.hwndTrack = hwnd;
          if (TrackMouseEvent(&tme)) {
            self->tracking_leave_ = true;
          }
        }
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
      self->client_.copy_frame_bgra(unused, fb_w, fb_h);
      if (fb_w > 0 && fb_h > 0) {
        if (!client_point_in_letterbox(hwnd, lparam, fb_w, fb_h)) {
          self->client_.set_software_cursor_enabled(false);
          return 0;
        }
        self->client_.set_software_cursor_enabled(true);
        self->client_.send_pointer(mask, map_mouse_x(hwnd, lparam, fb_w, fb_h),
                                   map_mouse_y(hwnd, lparam, fb_w, fb_h));
      }
      return 0;
    }
    case WM_KILLFOCUS:
    case WM_ACTIVATE:
      if (msg == WM_ACTIVATE && LOWORD(wparam) != WA_INACTIVE && self->floating_) {
        SessionHost::set_keyboard_target(self);
      }
      if (msg == WM_KILLFOCUS ||
          (msg == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE)) {
        self->client_.release_modifiers();
        self->client_.set_software_cursor_enabled(false);
      }
      break;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
      if (!g_kb_hook) {
        self->client_.send_vk(static_cast<unsigned>(wparam),
                              msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

SessionHost::SessionHost() = default;

SessionHost::~SessionHost() {
  close();
}

bool SessionHost::register_class(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = SessionWndProc;
  wc.hInstance = instance;
  wc.hCursor = nullptr;
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kSessionClass;
  if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  return true;
}

bool SessionHost::open(HINSTANCE instance, HWND parent_or_null, const std::wstring& title,
                       const ConnectDefaults& connect, ClosedFn on_closed) {
  if (hwnd_) {
    return false;
  }
  title_ = title;
  on_closed_ = std::move(on_closed);
  floating_ = (parent_or_null == nullptr);

  DWORD style = floating_ ? WS_OVERLAPPEDWINDOW : (WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
  DWORD ex = 0;
  hwnd_ = CreateWindowExW(ex, kSessionClass, title_.c_str(), style, 0, 0,
                          floating_ ? 1024 : 100, floating_ ? 768 : 100, parent_or_null, nullptr,
                          instance, this);
  if (!hwnd_) {
    return false;
  }
  AddClipboardFormatListener(hwnd_);

  road_desk::media::MediaClientConfig cfg;
  apply_connect_defaults_to(&cfg, connect);
  cfg.notify_hwnd = hwnd_;
  cfg.resize_msg = WM_MEDIA_RESIZE;
  if (cfg.require_tls && !cfg.tls_insecure && cfg.tls_fingerprint_sha256.empty()) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    return false;
  }
  if (!client_.start(cfg)) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    return false;
  }
  started_ = true;
  if (floating_) {
    ShowWindow(hwnd_, SW_SHOW);
  }
  return true;
}

void SessionHost::close() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    // on_destroy clears hwnd_
  } else if (started_) {
    client_.stop();
    started_ = false;
  }
}

bool SessionHost::connected() const {
  return started_ && client_.connected();
}

bool SessionHost::detach_to_floating(HINSTANCE instance) {
  if (!hwnd_ || floating_) {
    return floating_;
  }
  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  SetParent(hwnd_, nullptr);
  const LONG_PTR style =
      (GetWindowLongPtrW(hwnd_, GWL_STYLE) & ~(WS_CHILD)) | WS_OVERLAPPEDWINDOW;
  SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
  SetWindowPos(hwnd_, nullptr, rc.left + 40, rc.top + 40, rc.right - rc.left,
               rc.bottom - rc.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  SetWindowTextW(hwnd_, title_.c_str());
  floating_ = true;
  client_.set_notify_hwnd(hwnd_);
  (void)instance;
  return true;
}

bool SessionHost::attach_to_parent(HWND parent) {
  if (!hwnd_ || !parent || !floating_) {
    return false;
  }
  SetParent(hwnd_, parent);
  const LONG_PTR style =
      (GetWindowLongPtrW(hwnd_, GWL_STYLE) & ~(WS_OVERLAPPEDWINDOW)) | WS_CHILD | WS_VISIBLE;
  SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
  floating_ = false;
  client_.set_notify_hwnd(hwnd_);
  SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
  return true;
}

bool SessionHost::wants_keyboard() const {
  if (!hwnd_ || g_kb_target != this) {
    return false;
  }
  HWND fg = GetForegroundWindow();
  if (floating_) {
    return fg == hwnd_;
  }
  // Docked: console (root owner) must be foreground.
  return fg == GetAncestor(hwnd_, GA_ROOT);
}

bool SessionHost::send_vk(unsigned vk, bool down) {
  return client_.send_vk(vk, down);
}

void SessionHost::release_modifiers() {
  client_.release_modifiers();
}

SessionHost* SessionHost::from_hwnd(HWND hwnd) {
  if (!hwnd) {
    return nullptr;
  }
  return reinterpret_cast<SessionHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void SessionHost::install_keyboard_hook(HINSTANCE instance) {
  if (!g_kb_hook) {
    g_kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, low_level_keyboard, instance, 0);
  }
}

void SessionHost::uninstall_keyboard_hook() {
  if (g_kb_hook) {
    UnhookWindowsHookEx(g_kb_hook);
    g_kb_hook = nullptr;
  }
  g_kb_target = nullptr;
}

void SessionHost::set_keyboard_target(SessionHost* host) {
  if (g_kb_target && g_kb_target != host) {
    g_kb_target->release_modifiers();
  }
  g_kb_target = host;
}

void SessionHost::release_backbuffer() {
  if (back_dc_ && back_old_) {
    SelectObject(back_dc_, back_old_);
    back_old_ = nullptr;
  }
  if (back_bmp_) {
    DeleteObject(back_bmp_);
    back_bmp_ = nullptr;
  }
  if (back_dc_) {
    DeleteDC(back_dc_);
    back_dc_ = nullptr;
  }
  back_w_ = 0;
  back_h_ = 0;
}

bool SessionHost::ensure_backbuffer(HDC hdc, int cw, int ch) {
  if (cw <= 0 || ch <= 0) {
    return false;
  }
  if (back_dc_ && back_bmp_ && back_w_ == cw && back_h_ == ch) {
    return true;
  }
  release_backbuffer();
  back_dc_ = CreateCompatibleDC(hdc);
  back_bmp_ = CreateCompatibleBitmap(hdc, cw, ch);
  if (!back_dc_ || !back_bmp_) {
    release_backbuffer();
    return false;
  }
  back_old_ = SelectObject(back_dc_, back_bmp_);
  back_w_ = cw;
  back_h_ = ch;
  return true;
}

void SessionHost::paint() {
  PAINTSTRUCT ps{};
  HDC hdc = BeginPaint(hwnd_, &ps);

  std::vector<uint8_t> bgra;
  int w = 0;
  int h = 0;
  client_.copy_frame_bgra(bgra, w, h);

  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;
  if (cw <= 0 || ch <= 0 || !ensure_backbuffer(hdc, cw, ch)) {
    EndPaint(hwnd_, &ps);
    return;
  }

  FillRect(back_dc_, &rc, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

  RECT dest{};
  if (w > 0 && h > 0 && fit_rect(cw, ch, w, h, &dest) &&
      bgra.size() >= static_cast<size_t>(w) * h * 4) {
    const int dw = dest.right - dest.left;
    const int dh = dest.bottom - dest.top;
    const uint8_t* bits = bgra.data();
    int bits_w = w;
    int bits_h = h;
    if (dw != w || dh != h) {
      scale_bgra_.resize(static_cast<size_t>(dw) * dh * 4);
      scale_nearest_bgra(bgra.data(), w, h, scale_bgra_.data(), dw, dh);
      bits = scale_bgra_.data();
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
    SetDIBitsToDevice(back_dc_, dest.left, dest.top, bits_w, bits_h, 0, 0, 0, bits_h, bits,
                      &bmi, DIB_RGB_COLORS);
  } else {
    const wchar_t* msg = L"Connecting / waiting for framebuffer…";
    SetBkMode(back_dc_, TRANSPARENT);
    SetTextColor(back_dc_, RGB(220, 220, 220));
    DrawTextW(back_dc_, msg, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  BitBlt(hdc, 0, 0, cw, ch, back_dc_, 0, 0, SRCCOPY);
  EndPaint(hwnd_, &ps);
}

void SessionHost::on_destroy() {
  RemoveClipboardFormatListener(hwnd_);
  release_backbuffer();
  if (started_) {
    client_.stop();
    started_ = false;
  }
  if (g_kb_target == this) {
    g_kb_target = nullptr;
  }
  hwnd_ = nullptr;
  floating_ = false;
  // Never delete `this` synchronously from WndProc — notify owner on next pump.
  if (on_closed_) {
    ClosedFn fn = std::move(on_closed_);
    on_closed_ = nullptr;
    fn(this);
  }
}

}  // namespace road_desk::viewer
