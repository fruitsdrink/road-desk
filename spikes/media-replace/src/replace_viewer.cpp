// Track P G3: TLS + Control + Video display + Input send.
// TLS is owned only by the network thread after auth (UI queues input) — avoids
// WM_MOUSEMOVE blocking on tls_lock during large video reads (viewer 假死).

#include "log_util.h"
#include "mux.h"
#include "protocol.h"

#include "tls_schannel.h"

#include "miniz.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr UINT kMsgFrame = WM_APP + 40;
constexpr int kKeyQueueCap = 64;

struct KeyEvent {
  uint16_t vk = 0;
  uint8_t down = 0;
};

struct App {
  HWND hwnd = nullptr;
  road_desk::media::tls::TlsSession* tls = nullptr;
  CRITICAL_SECTION frame_lock{};
  CRITICAL_SECTION input_lock{};
  std::vector<uint8_t> pixels;
  int width = 0;
  int height = 0;
  int desk_w = 0;
  int desk_h = 0;
  uint32_t frame_id = 0;
  uint32_t frames_drawn = 0;
  volatile LONG stop = 0;
  volatile LONG paint_pending = 0;

  // Latest-pointer-wins + small key queue (UI -> net thread).
  bool ptr_pending = false;
  uint8_t ptr_buttons = 0;
  uint16_t ptr_x = 0;
  uint16_t ptr_y = 0;
  KeyEvent keys[kKeyQueueCap]{};
  int key_head = 0;
  int key_tail = 0;
  uint32_t input_flush_ok = 0;
  uint32_t input_coalesced = 0;

  std::vector<uint8_t> decode_buf;
  uint32_t rects_applied = 0;
  uint32_t zlib_ok = 0;
  uint32_t raw_ok = 0;
  uint32_t copy_ok = 0;
  uint32_t decode_fail = 0;
  uint64_t wire_bytes = 0;
  DWORD last_stat_ms = 0;
  HANDLE thread = nullptr;

  // Software-composited cursor (Win7 CreateIconIndirect alpha/XOR is unreliable).
  CRITICAL_SECTION cursor_lock{};
  std::vector<uint8_t> cursor_bgra;
  int cursor_w = 0;
  int cursor_h = 0;
  int cursor_hot_x = 0;
  int cursor_hot_y = 0;
  bool cursor_hidden = true;
  bool cursor_have = false;
  int local_mx = -1;
  int local_my = -1;
  uint32_t cursor_updates = 0;
};

App g_app;

void logf(const char* fmt, ...) {
  char line[2048];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  road_desk::replace::logf("replace-viewer", "%s", line);
}

bool env_truthy(const char* name) {
  char* env = nullptr;
  size_t len = 0;
  if (_dupenv_s(&env, &len, name) != 0 || !env) {
    return false;
  }
  const bool on = (env[0] == '1' || env[0] == 'y' || env[0] == 'Y' || env[0] == 't' ||
                   env[0] == 'T');
  free(env);
  return on;
}

bool wsa_init() {
  WSADATA wsa{};
  return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

uint16_t read_u16_le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t read_u32_le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void write_u16_le(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

void cursor_dest_rect(int mx, int my, int cw, int ch, int fw, int fh, int hot_x, int hot_y,
                      int cur_w, int cur_h, RECT* out) {
  if (!out || fw <= 0 || fh <= 0 || cw <= 0 || ch <= 0) {
    if (out) {
      SetRectEmpty(out);
    }
    return;
  }
  const int dw = (cur_w * cw + fw / 2) / fw;
  const int dh = (cur_h * ch + fh / 2) / fh;
  const int hx = (hot_x * cw + fw / 2) / fw;
  const int hy = (hot_y * ch + fh / 2) / fh;
  out->left = mx - hx;
  out->top = my - hy;
  out->right = out->left + (dw > 0 ? dw : 1);
  out->bottom = out->top + (dh > 0 ? dh : 1);
}

void invalidate_local_cursor(HWND hwnd, int old_mx, int old_my, int new_mx, int new_my) {
  if (!hwnd) {
    return;
  }
  RECT crc{};
  GetClientRect(hwnd, &crc);
  const int cw = crc.right - crc.left;
  const int ch = crc.bottom - crc.top;
  EnterCriticalSection(&g_app.frame_lock);
  const int fw = g_app.width > 0 ? g_app.width : g_app.desk_w;
  const int fh = g_app.height > 0 ? g_app.height : g_app.desk_h;
  LeaveCriticalSection(&g_app.frame_lock);
  EnterCriticalSection(&g_app.cursor_lock);
  const int cwgt = g_app.cursor_w;
  const int chgt = g_app.cursor_h;
  const int hx = g_app.cursor_hot_x;
  const int hy = g_app.cursor_hot_y;
  const bool draw = g_app.cursor_have && !g_app.cursor_hidden;
  LeaveCriticalSection(&g_app.cursor_lock);
  if (!draw || fw <= 0 || fh <= 0) {
    return;
  }
  RECT a{};
  RECT b{};
  if (old_mx >= 0 && old_my >= 0) {
    cursor_dest_rect(old_mx, old_my, cw, ch, fw, fh, hx, hy, cwgt, chgt, &a);
    InflateRect(&a, 1, 1);
    InvalidateRect(hwnd, &a, FALSE);
  }
  if (new_mx >= 0 && new_my >= 0) {
    cursor_dest_rect(new_mx, new_my, cw, ch, fw, fh, hx, hy, cwgt, chgt, &b);
    InflateRect(&b, 1, 1);
    InvalidateRect(hwnd, &b, FALSE);
  }
}

void draw_software_cursor(HDC hdc, int cw, int ch, int fw, int fh) {
  if (!hdc || fw <= 0 || fh <= 0 || cw <= 0 || ch <= 0) {
    return;
  }
  std::vector<uint8_t> bgra;
  int cur_w = 0;
  int cur_h = 0;
  int hot_x = 0;
  int hot_y = 0;
  int mx = -1;
  int my = -1;
  EnterCriticalSection(&g_app.cursor_lock);
  mx = g_app.local_mx;
  my = g_app.local_my;
  if (g_app.cursor_have && !g_app.cursor_hidden && mx >= 0 && my >= 0 &&
      !g_app.cursor_bgra.empty()) {
    bgra = g_app.cursor_bgra;
    cur_w = g_app.cursor_w;
    cur_h = g_app.cursor_h;
    hot_x = g_app.cursor_hot_x;
    hot_y = g_app.cursor_hot_y;
  }
  LeaveCriticalSection(&g_app.cursor_lock);
  if (bgra.empty() || cur_w <= 0 || cur_h <= 0) {
    return;
  }

  // Premultiplied BGRA for AlphaBlend(AC_SRC_ALPHA).
  std::vector<uint8_t> premul(bgra.size());
  for (size_t i = 0; i < bgra.size(); i += 4) {
    const uint8_t a = bgra[i + 3];
    premul[i + 0] = static_cast<uint8_t>((bgra[i + 0] * a) / 255);
    premul[i + 1] = static_cast<uint8_t>((bgra[i + 1] * a) / 255);
    premul[i + 2] = static_cast<uint8_t>((bgra[i + 2] * a) / 255);
    premul[i + 3] = a;
  }

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = cur_w;
  bmi.bmiHeader.biHeight = -cur_h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  HDC mem = CreateCompatibleDC(hdc);
  if (!mem) {
    return;
  }
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) {
      DeleteObject(dib);
    }
    DeleteDC(mem);
    return;
  }
  std::memcpy(bits, premul.data(), premul.size());
  HGDIOBJ old = SelectObject(mem, dib);

  RECT dr{};
  cursor_dest_rect(mx, my, cw, ch, fw, fh, hot_x, hot_y, cur_w, cur_h, &dr);
  const int dw = dr.right - dr.left;
  const int dh = dr.bottom - dr.top;
  BLENDFUNCTION bf{};
  bf.BlendOp = AC_SRC_OVER;
  bf.SourceConstantAlpha = 255;
  bf.AlphaFormat = AC_SRC_ALPHA;
  AlphaBlend(hdc, dr.left, dr.top, dw, dh, mem, 0, 0, cur_w, cur_h, bf);

  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
}

void apply_cursor_payload(const std::vector<uint8_t>& payload) {
  using namespace road_desk::replace;
  if (payload.size() < kCursorShapeHeaderSize || payload[0] != kCursorShape) {
    return;
  }
  const uint8_t flags = payload[1];
  const uint16_t hot_x = read_u16_le(payload.data() + 2);
  const uint16_t hot_y = read_u16_le(payload.data() + 4);
  const uint16_t w = read_u16_le(payload.data() + 6);
  const uint16_t h = read_u16_le(payload.data() + 8);
  bool hidden = (flags & kCursorFlagHidden) != 0;
  std::vector<uint8_t> bgra;
  if (!hidden && w > 0 && h > 0) {
    const size_t need = kCursorShapeHeaderSize + static_cast<size_t>(w) * h * 4u;
    if (payload.size() < need) {
      return;
    }
    bgra.assign(payload.data() + kCursorShapeHeaderSize,
                payload.data() + kCursorShapeHeaderSize + static_cast<size_t>(w) * h * 4u);
  } else {
    hidden = true;
  }

  EnterCriticalSection(&g_app.cursor_lock);
  g_app.cursor_bgra.swap(bgra);
  g_app.cursor_w = static_cast<int>(w);
  g_app.cursor_h = static_cast<int>(h);
  g_app.cursor_hot_x = static_cast<int>(hot_x);
  g_app.cursor_hot_y = static_cast<int>(hot_y);
  g_app.cursor_hidden = hidden;
  g_app.cursor_have = !hidden && !g_app.cursor_bgra.empty();
  ++g_app.cursor_updates;
  const int mx = g_app.local_mx;
  const int my = g_app.local_my;
  LeaveCriticalSection(&g_app.cursor_lock);

  if (g_app.hwnd) {
    invalidate_local_cursor(g_app.hwnd, mx, my, mx, my);
  }
}

bool socket_readable(SOCKET s, DWORD timeout_ms) {
  if (s == INVALID_SOCKET) {
    return false;
  }
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(s, &rfds);
  timeval tv{};
  tv.tv_sec = static_cast<long>(timeout_ms / 1000);
  tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
  const int r = select(0, &rfds, nullptr, nullptr, &tv);
  return r > 0 && FD_ISSET(s, &rfds);
}

void queue_pointer(int buttons, int x, int y) {
  EnterCriticalSection(&g_app.input_lock);
  if (g_app.ptr_pending) {
    ++g_app.input_coalesced;
  }
  g_app.ptr_pending = true;
  g_app.ptr_buttons = static_cast<uint8_t>(buttons & 0xff);
  g_app.ptr_x = static_cast<uint16_t>(x);
  g_app.ptr_y = static_cast<uint16_t>(y);
  LeaveCriticalSection(&g_app.input_lock);
}

void queue_key(unsigned vk, bool down) {
  EnterCriticalSection(&g_app.input_lock);
  const int next = (g_app.key_tail + 1) % kKeyQueueCap;
  if (next != g_app.key_head) {
    g_app.keys[g_app.key_tail].vk = static_cast<uint16_t>(vk);
    g_app.keys[g_app.key_tail].down = down ? 1 : 0;
    g_app.key_tail = next;
  }
  LeaveCriticalSection(&g_app.input_lock);
}

// Net thread only. Returns false if TLS write failed.
bool flush_input_queue() {
  using namespace road_desk::replace;
  if (!g_app.tls) {
    return false;
  }

  bool has_ptr = false;
  uint8_t buttons = 0;
  uint16_t x = 0;
  uint16_t y = 0;
  KeyEvent local_keys[kKeyQueueCap];
  int nkeys = 0;

  EnterCriticalSection(&g_app.input_lock);
  if (g_app.ptr_pending) {
    has_ptr = true;
    buttons = g_app.ptr_buttons;
    x = g_app.ptr_x;
    y = g_app.ptr_y;
    g_app.ptr_pending = false;
  }
  while (g_app.key_head != g_app.key_tail && nkeys < kKeyQueueCap) {
    local_keys[nkeys++] = g_app.keys[g_app.key_head];
    g_app.key_head = (g_app.key_head + 1) % kKeyQueueCap;
  }
  LeaveCriticalSection(&g_app.input_lock);

  if (has_ptr) {
    uint8_t body[kInputPointerSize];
    body[0] = kInputPointer;
    body[1] = buttons;
    write_u16_le(body + 2, x);
    write_u16_le(body + 4, y);
    if (!mux_write(g_app.tls, kChannelInput, body, kInputPointerSize)) {
      return false;
    }
    ++g_app.input_flush_ok;
  }
  for (int i = 0; i < nkeys; ++i) {
    uint8_t body[kInputKeySize];
    body[0] = kInputKey;
    body[1] = local_keys[i].down;
    write_u16_le(body + 2, local_keys[i].vk);
    if (!mux_write(g_app.tls, kChannelInput, body, kInputKeySize)) {
      return false;
    }
    ++g_app.input_flush_ok;
  }
  return true;
}

void map_mouse(HWND hwnd, LPARAM lparam, int* out_x, int* out_y) {
  EnterCriticalSection(&g_app.frame_lock);
  const int fb_w = g_app.width;
  const int fb_h = g_app.height;
  LeaveCriticalSection(&g_app.frame_lock);
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;
  int x = 0;
  int y = 0;
  if (fb_w > 0 && fb_h > 0 && cw > 0 && ch > 0) {
    x = (GET_X_LPARAM(lparam) * fb_w) / cw;
    y = (GET_Y_LPARAM(lparam) * fb_h) / ch;
    if (x < 0) {
      x = 0;
    }
    if (y < 0) {
      y = 0;
    }
    if (x >= fb_w) {
      x = fb_w - 1;
    }
    if (y >= fb_h) {
      y = fb_h - 1;
    }
  }
  *out_x = x;
  *out_y = y;
}

void paint(HWND hwnd) {
  PAINTSTRUCT ps{};
  HDC hdc = BeginPaint(hwnd, &ps);
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;

  EnterCriticalSection(&g_app.frame_lock);
  const int w = g_app.width;
  const int h = g_app.height;
  const uint8_t* pixels = nullptr;
  if (w > 0 && h > 0 && g_app.pixels.size() == static_cast<size_t>(w) * h * 4u) {
    pixels = g_app.pixels.data();
  }
  if (pixels && cw > 0 && ch > 0) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(hdc, COLORONCOLOR);
    // Paint under lock without 5.7MB memcpy (was starving UI + backing up TCP).
    StretchDIBits(hdc, 0, 0, cw, ch, 0, 0, w, h, pixels, &bmi, DIB_RGB_COLORS, SRCCOPY);
    ++g_app.frames_drawn;
    LeaveCriticalSection(&g_app.frame_lock);
    draw_software_cursor(hdc, cw, ch, w, h);
  } else {
    LeaveCriticalSection(&g_app.frame_lock);
    FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
  }
  EndPaint(hwnd, &ps);
}

void request_paint(HWND hwnd) {
  if (!hwnd) {
    return;
  }
  if (InterlockedCompareExchange(&g_app.paint_pending, 1, 0) == 0) {
    PostMessageW(hwnd, kMsgFrame, 0, 0);
  }
}

void on_mouse(HWND hwnd, WPARAM wparam, LPARAM lparam) {
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
  int x = 0;
  int y = 0;
  map_mouse(hwnd, lparam, &x, &y);
  queue_pointer(mask, x, y);

  const int cx = GET_X_LPARAM(lparam);
  const int cy = GET_Y_LPARAM(lparam);
  EnterCriticalSection(&g_app.cursor_lock);
  const int old_x = g_app.local_mx;
  const int old_y = g_app.local_my;
  g_app.local_mx = cx;
  g_app.local_my = cy;
  LeaveCriticalSection(&g_app.cursor_lock);
  invalidate_local_cursor(hwnd, old_x, old_y, cx, cy);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SETCURSOR:
      // Hide OS cursor in client; shape is AlphaBlend'd in paint (instant + correct).
      if (LOWORD(lp) == HTCLIENT) {
        EnterCriticalSection(&g_app.cursor_lock);
        if (g_app.cursor_have && !g_app.cursor_hidden) {
          SetCursor(nullptr);
        } else {
          SetCursor(LoadCursor(nullptr, IDC_ARROW));
        }
        LeaveCriticalSection(&g_app.cursor_lock);
        return TRUE;
      }
      return DefWindowProcW(hwnd, msg, wp, lp);
    case kMsgFrame:
      InterlockedExchange(&g_app.paint_pending, 0);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_PAINT:
      paint(hwnd);
      return 0;
    case WM_LBUTTONDOWN:
      SetCapture(hwnd);
      on_mouse(hwnd, wp, lp);
      return 0;
    case WM_LBUTTONUP:
      on_mouse(hwnd, wp, lp);
      ReleaseCapture();
      return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEMOVE:
      on_mouse(hwnd, wp, lp);
      return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
      queue_key(static_cast<unsigned>(wp), msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
      return 0;
    case WM_KILLFOCUS:
      queue_pointer(0, 0, 0);
      return 0;
    case WM_DESTROY:
      InterlockedExchange(&g_app.stop, 1);
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

bool ensure_fb_locked() {
  if (!g_app.pixels.empty()) {
    return true;
  }
  if (g_app.desk_w <= 0 || g_app.desk_h <= 0) {
    return false;
  }
  g_app.width = g_app.desk_w;
  g_app.height = g_app.desk_h;
  g_app.pixels.assign(static_cast<size_t>(g_app.width) * g_app.height * 4u, 0);
  return true;
}

bool apply_video_payload(const std::vector<uint8_t>& payload) {
  using namespace road_desk::replace;
  if (payload.size() < kVideoHeaderSize) {
    return true;
  }
  const uint8_t codec = payload[0];
  const uint32_t fid = read_u32_le(payload.data() + 1);
  const uint16_t x = read_u16_le(payload.data() + 5);
  const uint16_t y = read_u16_le(payload.data() + 7);
  const uint16_t rw = read_u16_le(payload.data() + 9);
  const uint16_t rh = read_u16_le(payload.data() + 11);
  if (rw == 0 || rh == 0) {
    return true;
  }

  if (!flush_input_queue()) {
    return false;
  }

  if (codec == kVideoCopyRect) {
    if (payload.size() < kCopyRectPayloadSize) {
      return true;
    }
    const uint16_t sx = read_u16_le(payload.data() + 13);
    const uint16_t sy = read_u16_le(payload.data() + 15);
    g_app.wire_bytes += kCopyRectPayloadSize;
    EnterCriticalSection(&g_app.frame_lock);
    if (!ensure_fb_locked()) {
      LeaveCriticalSection(&g_app.frame_lock);
      return true;
    }
    if (x + rw <= g_app.width && y + rh <= g_app.height && sx + rw <= g_app.width &&
        sy + rh <= g_app.height) {
      std::vector<uint8_t> tmp(static_cast<size_t>(rw) * rh * 4u);
      for (int row = 0; row < rh; ++row) {
        const uint8_t* src =
            g_app.pixels.data() + (static_cast<size_t>(sy + row) * g_app.width + sx) * 4u;
        std::memcpy(tmp.data() + static_cast<size_t>(row) * rw * 4u, src,
                    static_cast<size_t>(rw) * 4u);
      }
      for (int row = 0; row < rh; ++row) {
        uint8_t* dst =
            g_app.pixels.data() + (static_cast<size_t>(y + row) * g_app.width + x) * 4u;
        std::memcpy(dst, tmp.data() + static_cast<size_t>(row) * rw * 4u,
                    static_cast<size_t>(rw) * 4u);
      }
      g_app.frame_id = fid;
      ++g_app.rects_applied;
      ++g_app.copy_ok;
    }
    LeaveCriticalSection(&g_app.frame_lock);
    request_paint(g_app.hwnd);
  } else if (codec == kVideoRawBgra || codec == kVideoZlibBgra) {
    const size_t raw_bytes = static_cast<size_t>(rw) * rh * 4u;
    const uint8_t* wire = payload.data() + kVideoHeaderSize;
    const size_t wire_len = payload.size() - kVideoHeaderSize;
    const uint8_t* src = nullptr;
    g_app.wire_bytes += wire_len + kVideoHeaderSize;

    if (codec == kVideoRawBgra) {
      if (wire_len < raw_bytes) {
        ++g_app.decode_fail;
        return true;
      }
      src = wire;
      ++g_app.raw_ok;
    } else {
      g_app.decode_buf.resize(raw_bytes);
      mz_ulong out_len = static_cast<mz_ulong>(raw_bytes);
      if (mz_uncompress(g_app.decode_buf.data(), &out_len, wire, static_cast<mz_ulong>(wire_len)) !=
              MZ_OK ||
          out_len != raw_bytes) {
        ++g_app.decode_fail;
        return true;
      }
      src = g_app.decode_buf.data();
      ++g_app.zlib_ok;
    }

    EnterCriticalSection(&g_app.frame_lock);
    const bool full =
        (x == 0 && y == 0 && rw == g_app.desk_w && rh == g_app.desk_h && g_app.desk_w > 0);
    if (full) {
      g_app.width = rw;
      g_app.height = rh;
      g_app.pixels.assign(src, src + raw_bytes);
    } else {
      if (!ensure_fb_locked()) {
        LeaveCriticalSection(&g_app.frame_lock);
        return true;
      }
      if (x + rw <= g_app.width && y + rh <= g_app.height) {
        for (int row = 0; row < rh; ++row) {
          uint8_t* dst =
              g_app.pixels.data() + (static_cast<size_t>(y + row) * g_app.width + x) * 4u;
          std::memcpy(dst, src + static_cast<size_t>(row) * rw * 4u, static_cast<size_t>(rw) * 4u);
        }
      } else {
        LeaveCriticalSection(&g_app.frame_lock);
        return true;
      }
    }
    g_app.frame_id = fid;
    ++g_app.rects_applied;
    LeaveCriticalSection(&g_app.frame_lock);
    request_paint(g_app.hwnd);
  } else {
    return true;
  }

  const DWORD now = GetTickCount();
  if (g_app.last_stat_ms == 0) {
    g_app.last_stat_ms = now;
  }
  if (fid == 0 || (now - g_app.last_stat_ms) >= 1000) {
    g_app.last_stat_ms = now;
    logf("recv id=%u codec=%u rect=%u,%u %ux%u applied=%u copy=%u drawn=%u cur=%u flush=%u "
         "wire_B=%llu",
         fid, codec, x, y, rw, rh, g_app.rects_applied, g_app.copy_ok, g_app.frames_drawn,
         g_app.cursor_updates, g_app.input_flush_ok,
         static_cast<unsigned long long>(g_app.wire_bytes));
  }
  return true;
}

bool idle_flush_input(void*) {
  if (InterlockedCompareExchange(&g_app.stop, 0, 0) != 0) {
    return false;
  }
  return flush_input_queue();
}

DWORD WINAPI net_thread(void*) {
  using namespace road_desk::replace;
  std::vector<uint8_t> payload;
  while (InterlockedCompareExchange(&g_app.stop, 0, 0) == 0) {
    if (!flush_input_queue()) {
      logf("input flush failed - disconnect");
      break;
    }
    const SOCKET sock = road_desk::media::tls::tls_get_socket(g_app.tls);
    const int pending = road_desk::media::tls::tls_pending(g_app.tls);
    if (pending < 0) {
      logf("tls_pending failed");
      break;
    }
    if (pending == 0 && !socket_readable(sock, 5)) {
      continue;
    }
    uint8_t ch = 0;
    // Pump Input while waiting for the rest of a large video frame.
    if (!mux_read_idle(g_app.tls, &ch, &payload, idle_flush_input, nullptr)) {
      logf("recv ended");
      break;
    }
    if (!flush_input_queue()) {
      break;
    }
    if (ch == kChannelVideo) {
      if (!apply_video_payload(payload)) {
        break;
      }
    } else if (ch == kChannelCursor) {
      apply_cursor_payload(payload);
    }
  }
  InterlockedExchange(&g_app.stop, 1);
  if (g_app.hwnd) {
    PostMessageW(g_app.hwnd, WM_CLOSE, 0, 0);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  SetProcessDPIAware();
  road_desk::replace::log_open("replace_viewer.log");
  logf("boot build=cursor-soft");
  InitializeCriticalSection(&g_app.frame_lock);
  InitializeCriticalSection(&g_app.input_lock);
  InitializeCriticalSection(&g_app.cursor_lock);

  if (!wsa_init()) {
    logf("WSAStartup failed");
    road_desk::replace::log_close();
    return 1;
  }

  std::string host_port = "127.0.0.1:5902";
  std::string password = "road-desk";
  std::string fingerprint;
  if (argc >= 2 && argv[1] && argv[1][0]) {
    host_port = argv[1];
  }
  if (argc >= 3 && argv[2] && argv[2][0]) {
    password = argv[2];
  }
  if (argc >= 4 && argv[3] && argv[3][0]) {
    fingerprint = argv[3];
  }
  {
    char* e = nullptr;
    size_t n = 0;
    if (_dupenv_s(&e, &n, "ROAD_DESK_PSK") == 0 && e) {
      password = e;
      free(e);
    }
  }
  {
    char* e = nullptr;
    size_t n = 0;
    if (_dupenv_s(&e, &n, "ROAD_DESK_TLS_FINGERPRINT") == 0 && e) {
      fingerprint = e;
      free(e);
    }
  }
  const bool insecure = env_truthy("ROAD_DESK_TLS_INSECURE");

  std::string host;
  int port = 0;
  if (!road_desk::media::tls::parse_host_port(host_port, &host, &port)) {
    logf("bad host:port '%s'", host_port.c_str());
    road_desk::replace::log_close();
    return 1;
  }

  SOCKET tcp = road_desk::media::tls::tcp_connect(host.c_str(), port);
  if (tcp == INVALID_SOCKET) {
    logf("TCP connect failed %s:%d", host.c_str(), port);
    road_desk::replace::log_close();
    return 1;
  }
  if (!insecure && fingerprint.empty()) {
    logf("set ROAD_DESK_TLS_FINGERPRINT or pass fingerprint, or ROAD_DESK_TLS_INSECURE=1");
    closesocket(tcp);
    road_desk::replace::log_close();
    return 1;
  }

  g_app.tls = road_desk::media::tls::client_handshake(tcp, insecure, fingerprint);
  if (!g_app.tls) {
    logf("TLS handshake failed");
    road_desk::replace::log_close();
    return 1;
  }
  logf("TLS up peer_fp=%s", road_desk::media::tls::peer_fingerprint_sha256(g_app.tls).c_str());

  using namespace road_desk::replace;
  if (!control_send_auth(g_app.tls, password)) {
    logf("send auth failed");
    road_desk::media::tls::tls_close(g_app.tls);
    road_desk::replace::log_close();
    return 1;
  }

  uint8_t ch = 0;
  std::vector<uint8_t> payload;
  if (!mux_read(g_app.tls, &ch, &payload) || ch != kChannelControl || payload.empty()) {
    logf("no Control reply");
    road_desk::media::tls::tls_close(g_app.tls);
    road_desk::replace::log_close();
    return 1;
  }
  if (payload[0] == kCtrlAuthFail) {
    logf("auth rejected");
    road_desk::media::tls::tls_close(g_app.tls);
    road_desk::replace::log_close();
    return 2;
  }
  if (payload[0] != kCtrlAuthOk || payload.size() < 5) {
    logf("unexpected control");
    road_desk::media::tls::tls_close(g_app.tls);
    road_desk::replace::log_close();
    return 1;
  }
  g_app.desk_w = read_u16_le(payload.data() + 1);
  g_app.desk_h = read_u16_le(payload.data() + 3);
  logf("auth ok desktop=%dx%d - local cursor + CopyRect", g_app.desk_w, g_app.desk_h);

  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"RoadDeskReplaceViewer";
  wc.hCursor = nullptr;  // software cursor drawn in paint
  RegisterClassW(&wc);

  const int win_w = g_app.desk_w > 0 ? g_app.desk_w : 1280;
  const int win_h = g_app.desk_h > 0 ? g_app.desk_h : 720;
  g_app.hwnd =
      CreateWindowExW(0, wc.lpszClassName, L"Road Desk Replace Viewer (G3)",
                      WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, win_w + 16,
                      win_h + 39, nullptr, nullptr, wc.hInstance, nullptr);
  if (!g_app.hwnd) {
    logf("CreateWindow failed");
    road_desk::media::tls::tls_close(g_app.tls);
    road_desk::replace::log_close();
    return 1;
  }

  g_app.thread = CreateThread(nullptr, 0, net_thread, nullptr, 0, nullptr);
  if (!g_app.thread) {
    logf("net thread failed");
    DestroyWindow(g_app.hwnd);
    road_desk::media::tls::tls_close(g_app.tls);
    road_desk::replace::log_close();
    return 1;
  }

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  InterlockedExchange(&g_app.stop, 1);
  if (g_app.tls) {
    road_desk::media::tls::tls_close(g_app.tls);
    g_app.tls = nullptr;
  }
  if (g_app.thread) {
    WaitForSingleObject(g_app.thread, 3000);
    CloseHandle(g_app.thread);
  }
  logf("exit drawn=%u last_id=%u applied=%u copy=%u cur=%u flush=%u wire_B=%llu",
       g_app.frames_drawn, g_app.frame_id, g_app.rects_applied, g_app.copy_ok,
       g_app.cursor_updates, g_app.input_flush_ok, static_cast<unsigned long long>(g_app.wire_bytes));
  EnterCriticalSection(&g_app.cursor_lock);
  g_app.cursor_bgra.clear();
  g_app.cursor_have = false;
  LeaveCriticalSection(&g_app.cursor_lock);
  DeleteCriticalSection(&g_app.frame_lock);
  DeleteCriticalSection(&g_app.input_lock);
  DeleteCriticalSection(&g_app.cursor_lock);
  WSACleanup();
  road_desk::replace::log_close();
  return 0;
}
