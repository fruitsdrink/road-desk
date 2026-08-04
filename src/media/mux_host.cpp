// Mux media-plane host — TLS Video/Input; capture = Mirror dirties (default) or GDI/DXGI.
// Ported from spikes/media-replace/src/replace_host.cpp.
// Three threads (VNC-style): inject (TLS read/SendInput), video-out (mux_write), encode (capture).

#include "media_plane.h"

#include "auth.h"
#include "capture_resolve.h"
#include "cursor_capture.h"
#include "jpeg_encode.h"
#include "h264_mf.h"
#include "media_log.h"
#include "mirror_capture.h"
#include "mux.h"
#include "mux_clipboard.h"
#include "mux_file_xfer.h"
#include "mux_inject.h"
#include "mux_protocol.h"
#include "os_version.h"
#include "product_version.h"
#include "session_mutex.h"
#include "session_recorder.h"
#include "tls_schannel.h"
#include "video_encode.h"

#include "mirror_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace road_desk::media {
namespace {

void logf(const char* fmt, ...) {
  char line[2048];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  media_logf("media-host", "%s", line);
}

bool wsa_init() {
  WSADATA wsa{};
  return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

// Aero Shake (title-bar shake → minimize others) fires on jumpy remote absolute mouse.
// DisallowShaking=1 does not disable Snap; restore previous value when capture ends.
struct ShakeGuard {
  bool active = false;
  bool had_value = false;
  DWORD prev = 0;
};

bool set_disallow_shaking(DWORD value) {
  HKEY key = nullptr;
  const LONG open = RegOpenKeyExW(
      HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", 0,
      KEY_SET_VALUE, &key);
  if (open != ERROR_SUCCESS) {
    return false;
  }
  const LONG st =
      RegSetValueExW(key, L"DisallowShaking", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                     sizeof(value));
  RegCloseKey(key);
  return st == ERROR_SUCCESS;
}

void aero_shake_disable(ShakeGuard* g) {
  if (!g || g->active) {
    return;
  }
  HKEY key = nullptr;
  const LONG open = RegOpenKeyExW(
      HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", 0,
      KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
  if (open != ERROR_SUCCESS) {
    logf("aero-shake: open Advanced failed (%ld)", open);
    return;
  }
  DWORD type = 0;
  DWORD cb = sizeof(g->prev);
  const LONG q = RegQueryValueExW(key, L"DisallowShaking", nullptr, &type,
                                  reinterpret_cast<BYTE*>(&g->prev), &cb);
  g->had_value = (q == ERROR_SUCCESS && type == REG_DWORD && cb == sizeof(DWORD));
  const DWORD one = 1;
  const LONG st =
      RegSetValueExW(key, L"DisallowShaking", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one),
                     sizeof(one));
  RegCloseKey(key);
  if (st != ERROR_SUCCESS) {
    logf("aero-shake: DisallowShaking=1 failed (%ld)", st);
    return;
  }
  g->active = true;
  logf("aero-shake: DisallowShaking=1 (was %s)",
       g->had_value ? (g->prev ? "1" : "0") : "unset");
}

void aero_shake_restore(ShakeGuard* g) {
  if (!g || !g->active) {
    return;
  }
  if (g->had_value) {
    if (!set_disallow_shaking(g->prev)) {
      logf("aero-shake: restore DisallowShaking=%u failed", static_cast<unsigned>(g->prev));
    } else {
      logf("aero-shake: restored DisallowShaking=%u", static_cast<unsigned>(g->prev));
    }
  } else {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", 0,
                      KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
      RegDeleteValueW(key, L"DisallowShaking");
      RegCloseKey(key);
      logf("aero-shake: deleted DisallowShaking (was unset)");
    }
  }
  g->active = false;
  g->had_value = false;
  g->prev = 0;
}

void write_u16_le(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

void write_u32_le(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

uint16_t read_u16_le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

bool rect_in_desk(int desk_w, int desk_h, int x, int y, int rw, int rh) {
  if (desk_w <= 0 || desk_h <= 0 || rw <= 0 || rh <= 0) {
    return false;
  }
  if (x < 0 || y < 0) {
    return false;
  }
  if (x > desk_w - rw || y > desk_h - rh) {
    return false;
  }
  return true;
}

struct DirtyRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

void copy_rect_to_prev(uint8_t* prev, const uint8_t* cur, int desk_w, int desk_h,
                       const DirtyRect& r) {
  if (!prev || !cur || !rect_in_desk(desk_w, desk_h, r.x, r.y, r.w, r.h)) {
    return;
  }
  for (int row = 0; row < r.h; ++row) {
    const size_t off = (static_cast<size_t>(r.y + row) * desk_w + r.x) * 4u;
    std::memcpy(prev + off, cur + off, static_cast<size_t>(r.w) * 4u);
  }
}

void coalesce_vertical(std::vector<DirtyRect>* rects) {
  if (!rects || rects->size() < 2) {
    return;
  }
  std::sort(rects->begin(), rects->end(), [](const DirtyRect& a, const DirtyRect& b) {
    if (a.y != b.y) {
      return a.y < b.y;
    }
    return a.x < b.x;
  });
  std::vector<DirtyRect> out;
  out.reserve(rects->size());
  for (const DirtyRect& r : *rects) {
    if (!out.empty()) {
      DirtyRect& last = out.back();
      if (last.x == r.x && last.w == r.w && last.y + last.h == r.y) {
        last.h += r.h;
        continue;
      }
    }
    out.push_back(r);
  }
  *rects = std::move(out);
}

void collect_dirty_rects(const uint8_t* cur, const uint8_t* prev, int desk_w, int desk_h,
                         bool force_full, std::vector<DirtyRect>* out) {
  out->clear();
  if (force_full || !prev) {
    out->push_back({0, 0, desk_w, desk_h});
    return;
  }
  constexpr int kBlock = 32;
  for (int y = 0; y < desk_h; y += kBlock) {
    const int yh = (y + kBlock < desk_h) ? (y + kBlock) : desk_h;
    int run_x0 = -1;
    int run_x1 = -1;
    for (int x = 0; x < desk_w; x += kBlock) {
      const int xw = (x + kBlock < desk_w) ? (x + kBlock) : desk_w;
      bool dirty = false;
      for (int yy = y; yy < yh && !dirty; ++yy) {
        const uint8_t* a = cur + (static_cast<size_t>(yy) * desk_w + x) * 4u;
        const uint8_t* b = prev + (static_cast<size_t>(yy) * desk_w + x) * 4u;
        if (std::memcmp(a, b, static_cast<size_t>(xw - x) * 4u) != 0) {
          dirty = true;
        }
      }
      if (dirty) {
        if (run_x0 < 0) {
          run_x0 = x;
        }
        run_x1 = xw;
      } else if (run_x0 >= 0) {
        out->push_back({run_x0, y, run_x1 - run_x0, yh - y});
        run_x0 = -1;
        run_x1 = -1;
      }
    }
    if (run_x0 >= 0) {
      out->push_back({run_x0, y, run_x1 - run_x0, yh - y});
    }
  }
  coalesce_vertical(out);
}

void collapse_dirties_for_send(std::vector<DirtyRect>* rects, int desk_w, int desk_h,
                               bool drag_mode) {
  if (!rects || rects->empty() || desk_w <= 0 || desk_h <= 0) {
    return;
  }
  if (drag_mode) {
    // One bbox while dragging: many small tiles + drop → staircase ghosts on Win7.
    if (rects->size() == 1) {
      return;
    }
    int x0 = desk_w;
    int y0 = desk_h;
    int x1 = 0;
    int y1 = 0;
    for (const DirtyRect& r : *rects) {
      if (r.x < x0) {
        x0 = r.x;
      }
      if (r.y < y0) {
        y0 = r.y;
      }
      if (r.x + r.w > x1) {
        x1 = r.x + r.w;
      }
      if (r.y + r.h > y1) {
        y1 = r.y + r.h;
      }
    }
    if (x1 > x0 && y1 > y0) {
      rects->assign(1, {x0, y0, x1 - x0, y1 - y0});
    }
    return;
  }
  if (rects->size() <= 1) {
    return;
  }
  size_t area = 0;
  for (const DirtyRect& r : *rects) {
    area += static_cast<size_t>(r.w) * static_cast<size_t>(r.h);
  }
  const size_t screen = static_cast<size_t>(desk_w) * static_cast<size_t>(desk_h);
  if (area * 2u > screen) {
    rects->assign(1, {0, 0, desk_w, desk_h});
    return;
  }
  if (rects->size() <= 24) {
    return;
  }
  int x0 = desk_w;
  int y0 = desk_h;
  int x1 = 0;
  int y1 = 0;
  for (const DirtyRect& r : *rects) {
    if (r.x < x0) {
      x0 = r.x;
    }
    if (r.y < y0) {
      y0 = r.y;
    }
    if (r.x + r.w > x1) {
      x1 = r.x + r.w;
    }
    if (r.y + r.h > y1) {
      y1 = r.y + r.h;
    }
  }
  if (x1 > x0 && y1 > y0) {
    rects->assign(1, {x0, y0, x1 - x0, y1 - y0});
  }
}

struct CopyRect {
  int sx = 0;
  int sy = 0;
  int dx = 0;
  int dy = 0;
  int w = 0;
  int h = 0;
};

bool send_copy_rect(tls::TlsSession* tls, uint32_t frame_id, const CopyRect& r) {
  using namespace road_desk::replace;
  uint8_t body[kCopyRectPayloadSize];
  body[0] = kVideoCopyRect;
  write_u32_le(body + 1, frame_id);
  write_u16_le(body + 5, static_cast<uint16_t>(r.dx));
  write_u16_le(body + 7, static_cast<uint16_t>(r.dy));
  write_u16_le(body + 9, static_cast<uint16_t>(r.w));
  write_u16_le(body + 11, static_cast<uint16_t>(r.h));
  write_u16_le(body + 13, static_cast<uint16_t>(r.sx));
  write_u16_le(body + 15, static_cast<uint16_t>(r.sy));
  return mux_write(tls, kChannelVideo, body, static_cast<uint32_t>(kCopyRectPayloadSize));
}

uint32_t g_input_pointer = 0;
uint32_t g_input_key = 0;
std::atomic<int> g_ptr_buttons{0};
std::atomic<int> g_ptr_buttons_prev{0};
std::atomic<int> g_ptr_x{0};
std::atomic<int> g_ptr_y{0};
std::atomic<int> g_ptr_x_prev{0};
std::atomic<int> g_ptr_y_prev{0};
std::atomic<bool> g_ptr_have_prev{false};
std::atomic<int> g_validate_frames{0};
std::atomic<bool> g_any_need_keyframe{false};
// Encode finished a keyframe batch — inject thread clears per-client need_keyframe.
std::atomic<bool> g_clear_client_keyframes{false};
// Inject thread is inside drain/SendInput — video-out should yield the IO lock.
std::atomic<bool> g_inject_active{false};
uint32_t g_cursor_hash_sent = 0;
std::atomic<uint32_t> g_copyrects_sent{0};
int g_host_cursor_hide = 0;
bool g_system_cursors_blanked = false;
std::atomic<int> g_video_ptr_x{0};
std::atomic<int> g_video_ptr_y{0};
std::atomic<bool> g_video_ptr_have{false};

// UltraVNC-style: replace system cursors with a blank cursor for the session so the
// Host pointer never appears in DXGI/Mirror frames (that was the Win10 "title-bar cursor").
void blank_system_cursors_for_session() {
  if (g_system_cursors_blanked) {
    return;
  }
  BYTE and_mask[128];
  BYTE xor_mask[128];
  std::memset(and_mask, 0xFF, sizeof(and_mask));
  std::memset(xor_mask, 0x00, sizeof(xor_mask));
  HCURSOR blank = CreateCursor(GetModuleHandleW(nullptr), 0, 0, 32, 32, and_mask, xor_mask);
  if (!blank) {
    // Fallback: per-thread hide (weaker; may not scrub cursor out of all paths).
    for (;;) {
      const int c = ShowCursor(FALSE);
      ++g_host_cursor_hide;
      if (c < 0 || g_host_cursor_hide > 64) {
        break;
      }
    }
    return;
  }
  // OCR_* ids (winuser.h OEMRESOURCE). Numeric to avoid OEMRESOURCE include order issues.
  static const UINT kIds[] = {
      32512,  // OCR_NORMAL
      32513,  // OCR_IBEAM
      32514,  // OCR_WAIT
      32515,  // OCR_CROSS
      32516,  // OCR_UP
      32642,  // OCR_SIZENWSE
      32643,  // OCR_SIZENESW
      32644,  // OCR_SIZEWE
      32645,  // OCR_SIZENS
      32646,  // OCR_SIZEALL
      32648,  // OCR_NO
      32649,  // OCR_HAND
      32650,  // OCR_APPSTARTING
  };
  for (UINT id : kIds) {
    HCURSOR copy = CopyCursor(blank);
    if (copy) {
      SetSystemCursor(copy, id);  // consumes copy
    }
  }
  DestroyCursor(blank);
  g_system_cursors_blanked = true;
  logf("host cursor: blanked system cursors for session");
}

void restore_system_cursors_after_session() {
  // Always reload OEM cursors — heals crash leftover even if our flag was lost.
  SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
  if (g_system_cursors_blanked) {
    g_system_cursors_blanked = false;
    logf("host cursor: restored system cursors");
  }
  while (g_host_cursor_hide > 0) {
    ShowCursor(TRUE);
    --g_host_cursor_hide;
  }
}

void hide_host_cursor_for_session() {
  blank_system_cursors_for_session();
}

void restore_host_cursor_after_session() {
  restore_system_cursors_after_session();
}

bool send_cursor_shape(tls::TlsSession* tls, const road_desk::replace::CursorShape& c) {
  using namespace road_desk::replace;
  if (c.hidden || c.w == 0 || c.h == 0 || c.bgra.empty()) {
    uint8_t body[kCursorShapeHeaderSize];
    body[0] = kCursorShape;
    body[1] = kCursorFlagHidden;
    write_u16_le(body + 2, 0);
    write_u16_le(body + 4, 0);
    write_u16_le(body + 6, 0);
    write_u16_le(body + 8, 0);
    return mux_write(tls, kChannelCursor, body, static_cast<uint32_t>(kCursorShapeHeaderSize));
  }
  const uint32_t pix = static_cast<uint32_t>(c.w) * c.h * 4u;
  const uint32_t total = static_cast<uint32_t>(kCursorShapeHeaderSize) + pix;
  if (total > kMaxPayloadLen) {
    return false;
  }
  std::vector<uint8_t> body(total);
  body[0] = kCursorShape;
  body[1] = 0;
  write_u16_le(body.data() + 2, c.hot_x);
  write_u16_le(body.data() + 4, c.hot_y);
  write_u16_le(body.data() + 6, c.w);
  write_u16_le(body.data() + 8, c.h);
  std::memcpy(body.data() + kCursorShapeHeaderSize, c.bgra.data(), pix);
  return mux_write(tls, kChannelCursor, body.data(), total);
}

bool send_cursor_if_changed(tls::TlsSession* tls, const road_desk::replace::CursorShape& shape,
                            uint32_t* hash_sent) {
  if (!hash_sent) {
    return false;
  }
  if (shape.hash == *hash_sent) {
    return true;
  }
  if (!send_cursor_shape(tls, shape)) {
    return false;
  }
  *hash_sent = shape.hash;
  return true;
}

void split_rects_for_input_slices(std::vector<DirtyRect>* rects, int max_h) {
  if (!rects || max_h <= 0) {
    return;
  }
  std::vector<DirtyRect> out;
  out.reserve(rects->size() * 2);
  for (const DirtyRect& r : *rects) {
    if (r.h <= max_h) {
      out.push_back(r);
      continue;
    }
    for (int y = r.y; y < r.y + r.h;) {
      const int slice_h = (r.y + r.h - y > max_h) ? max_h : (r.y + r.h - y);
      out.push_back({r.x, y, r.w, slice_h});
      y += slice_h;
    }
  }
  *rects = std::move(out);
}

void handle_input(const std::vector<uint8_t>& payload) {
  using namespace road_desk::replace;
  if (payload.empty()) {
    return;
  }
  if (payload[0] == kInputPointer && payload.size() >= kInputPointerSize) {
    ++g_input_pointer;
    const int buttons = payload[1];
    const int prev_btns = g_ptr_buttons_prev.load();
    g_ptr_buttons.store(buttons);
    if ((prev_btns & 1) != (buttons & 1)) {
      g_validate_frames.store(2);
    }
    g_ptr_buttons_prev.store(buttons);
    const int x = read_u16_le(payload.data() + 2);
    const int y = read_u16_le(payload.data() + 4);
    if (g_ptr_have_prev.load()) {
      g_ptr_x_prev.store(g_ptr_x.load());
      g_ptr_y_prev.store(g_ptr_y.load());
    } else {
      g_ptr_x_prev.store(x);
      g_ptr_y_prev.store(y);
      g_ptr_have_prev.store(true);
    }
    g_ptr_x.store(x);
    g_ptr_y.store(y);
    inject_pointer(buttons, static_cast<uint16_t>(x), static_cast<uint16_t>(y));
    return;
  }
  if (payload[0] == kInputKey && payload.size() >= kInputKeySize) {
    ++g_input_key;
    const bool down = (payload[1] & kInputKeyFlagDown) != 0;
    const bool extended = (payload[1] & kInputKeyFlagExtended) != 0;
    inject_vk(read_u16_le(payload.data() + 2), down, extended);
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

bool socket_writable(SOCKET s, DWORD timeout_ms) {
  if (s == INVALID_SOCKET) {
    return false;
  }
  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(s, &wfds);
  timeval tv{};
  tv.tv_sec = static_cast<long>(timeout_ms / 1000);
  tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
  const int r = select(0, nullptr, &wfds, nullptr, &tv);
  return r > 0 && FD_ISSET(s, &wfds);
}

struct HostClipSession {
  road_desk::replace::ClipboardEchoGuard echo;
  DWORD last_seq = 0;
  uint32_t next_xfer_id = 1;
  bool sending_files = false;
  road_desk::replace::FileRecvState recv;
};

bool send_file_abort(tls::TlsSession* tls, uint32_t xfer_id) {
  using namespace road_desk::replace;
  uint8_t body[5];
  body[0] = kFileAbort;
  write_u32_le(body + 1, xfer_id);
  return mux_write(tls, kChannelFile, body, 5);
}

bool drain_incoming(tls::TlsSession* tls, HostClipSession* clip) {
  using namespace road_desk::replace;
  const SOCKET sock = tls::tls_get_socket(tls);
  while (socket_readable(sock, 0)) {
    uint8_t ch = 0;
    std::vector<uint8_t> payload;
    if (!mux_read(tls, &ch, &payload)) {
      return false;
    }
    if (ch == kChannelInput) {
      handle_input(payload);
      continue;
    }
    if (ch == kChannelControl && !payload.empty() && payload[0] == kCtrlPing) {
      uint8_t pong = kCtrlPong;
      if (!mux_write(tls, kChannelControl, &pong, 1)) {
        return false;
      }
      continue;
    }
    if (ch == kChannelClipboard && clip && !payload.empty()) {
      if (payload[0] == kClipText) {
        std::vector<uint8_t> utf16;
        if (parse_clip_text_payload(payload.data(), payload.size(), &utf16)) {
          if (clipboard_write_text_utf16(utf16.data(), utf16.size(), &clip->echo)) {
            clip->last_seq = clip->echo.ignore_seq;
            logf("clipboard text in %zu bytes", utf16.size());
          }
        } else {
          logf("clipboard text parse failed len=%zu", payload.size());
        }
        continue;
      }
      if (payload[0] == kClipBitmap) {
        std::vector<uint8_t> dib;
        if (parse_clip_bitmap_payload(payload.data(), payload.size(), &dib)) {
          if (clipboard_write_dib(dib.data(), dib.size(), &clip->echo)) {
            clip->last_seq = clip->echo.ignore_seq;
            logf("clipboard bitmap in %zu bytes", dib.size());
          }
        } else {
          logf("clipboard bitmap parse failed len=%zu", payload.size());
        }
        continue;
      }
      if (payload[0] == kClipFilesOffer) {
        FileOffer offer;
        if (!parse_clip_files_offer_payload(payload.data(), payload.size(), &offer)) {
          logf("clipboard files offer parse failed len=%zu", payload.size());
          continue;
        }
        if (clip->recv.active) {
          send_file_abort(tls, clip->recv.offer.transfer_id);
          file_recv_abort(&clip->recv);
        }
        std::string err;
        if (!file_recv_begin(&clip->recv, offer, &err)) {
          logf("clipboard files recv begin failed: %s", err.c_str());
          send_file_abort(tls, offer.transfer_id);
        } else {
          logf("clipboard files offer id=%u entries=%u files=%u", offer.transfer_id,
               static_cast<unsigned>(offer.entries.size()), offer.file_count);
        }
        continue;
      }
      if (payload[0] == kClipClear) {
        continue;
      }
      continue;
    }
    if (ch == kChannelFile && clip && !payload.empty()) {
      DWORD echo_seq = 0;
      std::string err;
      if (!file_recv_on_payload(&clip->recv, payload.data(), payload.size(), &echo_seq, &err)) {
        logf("file recv failed: %s", err.empty() ? "error" : err.c_str());
        if (clip->recv.offer.transfer_id) {
          send_file_abort(tls, clip->recv.offer.transfer_id);
        }
        file_recv_abort(&clip->recv);
        // Keep session up on xfer errors.
        continue;
      }
      if (echo_seq != 0) {
        clip->echo.ignore_seq = echo_seq;
        clip->last_seq = echo_seq;
        logf("clipboard files published id=%u", clip->recv.offer.transfer_id);
      }
      continue;
    }
  }
  return true;
}

bool pump_clip_drain(tls::TlsSession* tls, HostClipSession* clip) {
  return drain_incoming(tls, clip);
}

constexpr size_t kMaxClients = 8;

struct ClientConn {
  tls::TlsSession* tls = nullptr;
  bool authed = false;
  bool need_keyframe = false;
  bool dead = false;
  uint32_t cursor_hash_sent = 0;
  HostClipSession clip;
};

void close_client(ClientConn* c, session::SessionMutex* mutex) {
  if (!c) {
    return;
  }
  road_desk::replace::file_recv_abort(&c->clip.recv);
  if (c->tls) {
    tls::tls_close(c->tls);
    c->tls = nullptr;
  }
  if (c->authed && mutex) {
    mutex->release();
  }
  c->authed = false;
  c->dead = true;
}

unsigned count_authed(const std::vector<ClientConn>& clients) {
  unsigned n = 0;
  for (const ClientConn& c : clients) {
    if (c.authed && !c.dead) {
      ++n;
    }
  }
  return n;
}

bool try_auth_client(ClientConn* c, const std::string& psk, session::SessionMutex* mutex) {
  using namespace road_desk::replace;
  if (!c || !c->tls || c->authed) {
    return true;
  }
  const SOCKET sock = tls::tls_get_socket(c->tls);
  const int pending = tls::tls_pending(c->tls);
  if (!socket_readable(sock, 0) && pending <= 0) {
    return true;
  }
  uint8_t ch = 0;
  std::vector<uint8_t> payload;
  if (!mux_read(c->tls, &ch, &payload)) {
    logf("auth read failed");
    return false;
  }
  if (ch != kChannelControl || payload.empty()) {
    logf("expected Control auth, got channel=%u", static_cast<unsigned>(ch));
    control_send_auth_fail(c->tls, "expected control auth");
    return false;
  }
  std::string provided;
  if (!parse_auth_password(payload.data(), payload.size(), &provided)) {
    control_send_auth_fail(c->tls, "malformed auth");
    return false;
  }
  if (!road_desk::session::authenticate_psk(psk, provided)) {
    control_send_auth_fail(c->tls, "auth failed");
    return false;
  }
  const int sw = GetSystemMetrics(SM_CXSCREEN);
  const int sh = GetSystemMetrics(SM_CYSCREEN);
  const uint16_t width = static_cast<uint16_t>(sw > 0 ? sw : 1);
  const uint16_t height = static_cast<uint16_t>(sh > 0 ? sh : 1);
  if (!control_send_auth_ok(c->tls, width, height)) {
    logf("send AuthOk failed");
    return false;
  }
  c->authed = true;
  c->need_keyframe = true;
  c->clip.last_seq = GetClipboardSequenceNumber();
  c->cursor_hash_sent = 0;
  if (mutex) {
    mutex->try_acquire();
  }
  logf("client authed desktop=%ux%u shared-control viewers=%u", width, height,
       mutex ? mutex->count() : 0);
  return true;
}

bool drain_all_clients(std::vector<ClientConn>* clients, const std::string& psk,
                       session::SessionMutex* mutex) {
  if (!clients) {
    return true;
  }
  for (ClientConn& c : *clients) {
    if (c.dead || !c.tls) {
      continue;
    }
    if (!c.authed) {
      if (!try_auth_client(&c, psk, mutex)) {
        close_client(&c, mutex);
      }
      continue;
    }
    if (!drain_incoming(c.tls, &c.clip)) {
      logf("client gone (recv)");
      close_client(&c, mutex);
    }
  }
  clients->erase(std::remove_if(clients->begin(), clients->end(),
                                [](const ClientConn& c) { return c.dead || !c.tls; }),
                 clients->end());
  return true;
}

bool push_clipboard_one(ClientConn* c, uint32_t* shared_xfer_id,
                        std::vector<ClientConn>* all_for_pump, const std::string& psk,
                        session::SessionMutex* mutex) {
  using namespace road_desk::replace;
  if (!c || !c->tls || !c->authed || c->clip.sending_files) {
    return true;
  }
  const DWORD seq = GetClipboardSequenceNumber();
  if (seq == 0 || seq == c->clip.last_seq) {
    return true;
  }
  if (c->clip.echo.should_skip_seq(seq)) {
    c->clip.last_seq = seq;
    return true;
  }

  auto pump = [&]() -> bool {
    return drain_all_clients(all_for_pump, psk, mutex);
  };

  if (clipboard_has_hdrop()) {
    FileOffer offer;
    std::string err;
    const uint32_t xid = shared_xfer_id ? (*shared_xfer_id)++ : c->clip.next_xfer_id++;
    if (!clipboard_build_file_offer(xid, &offer, &err)) {
      c->clip.last_seq = seq;
      return true;
    }
    std::vector<uint8_t> payload;
    if (!build_clip_files_offer_payload(offer, &payload)) {
      c->clip.last_seq = seq;
      return true;
    }
    if (!mux_write(c->tls, kChannelClipboard, payload.data(),
                   static_cast<uint32_t>(payload.size()))) {
      return false;
    }
    c->clip.last_seq = seq;
    c->clip.sending_files = true;
    const bool ok = file_xfer_send_all(c->tls, offer, pump, &err);
    c->clip.sending_files = false;
    if (!ok) {
      logf("clipboard files send failed: %s", err.c_str());
      send_file_abort(c->tls, offer.transfer_id);
      return err != "pump abort";
    }
    return true;
  }

  if (clipboard_has_dib()) {
    std::vector<uint8_t> dib;
    if (!clipboard_read_dib(&dib)) {
      c->clip.last_seq = seq;
      return true;
    }
    std::vector<uint8_t> payload;
    if (!build_clip_bitmap_payload(dib, &payload)) {
      c->clip.last_seq = seq;
      return true;
    }
    if (!mux_write(c->tls, kChannelClipboard, payload.data(),
                   static_cast<uint32_t>(payload.size()))) {
      return false;
    }
    c->clip.last_seq = seq;
    return true;
  }

  if (clipboard_has_text()) {
    std::vector<uint8_t> utf16;
    if (!clipboard_read_text_utf16(&utf16)) {
      c->clip.last_seq = seq;
      return true;
    }
    std::vector<uint8_t> payload;
    if (!build_clip_text_payload(utf16, &payload)) {
      c->clip.last_seq = seq;
      return true;
    }
    if (!mux_write(c->tls, kChannelClipboard, payload.data(),
                   static_cast<uint32_t>(payload.size()))) {
      return false;
    }
    c->clip.last_seq = seq;
    return true;
  }

  c->clip.last_seq = seq;
  return true;
}

bool fanout_host_clipboard(std::vector<ClientConn>* clients, uint32_t* shared_xfer_id,
                           const std::string& psk, session::SessionMutex* mutex) {
  if (!clients) {
    return true;
  }
  for (ClientConn& c : *clients) {
    if (!c.authed || c.dead) {
      continue;
    }
    if (!push_clipboard_one(&c, shared_xfer_id, clients, psk, mutex)) {
      close_client(&c, mutex);
    }
  }
  clients->erase(std::remove_if(clients->begin(), clients->end(),
                                [](const ClientConn& c) { return c.dead || !c.tls; }),
                 clients->end());
  return true;
}

void serve_shared(SOCKET listen_sock, const std::string& psk,
                  road_desk::replace::CaptureMode capture_mode,
                  road_desk::replace::CaptureStrategy capture_strategy,
                  const std::atomic<bool>* stop_requested, session::SessionMutex* mutex,
                  tls::HostCredentials* creds) {
  using namespace road_desk::replace;

  struct OutBatch {
    std::vector<std::vector<uint8_t>> msgs;
  };

  struct SharedIo {
    std::mutex io_mu;    // clients + all TLS read/write
    std::mutex outq_mu;  // encode push / video-out pop
    std::deque<OutBatch> outq;
    std::vector<ClientConn> clients;
    std::atomic<bool> session_active{false};
    std::atomic<bool> encode_stop{false};
    std::atomic<bool> video_out_stop{false};
    // Mid large video TLS write — inject may drain/SendInput but must not mux_write.
    std::atomic<bool> video_writing{false};
    std::atomic<uint32_t> dropped_ticks{0};
    std::atomic<uint32_t> dropped_interval{0};
  };

  SharedIo shared;
  shared.clients.reserve(kMaxClients);
  constexpr size_t kMaxBatches = 2;
  const bool modern_lossy = (capture_strategy == CaptureStrategy::Modern);
  uint32_t shared_xfer_id = 1;

  struct VideoWriteYieldCtx {
    SharedIo* shared = nullptr;
    std::unique_lock<std::mutex>* lock = nullptr;
  };
  auto video_write_yield = [](void* raw) -> bool {
    auto* ctx = static_cast<VideoWriteYieldCtx*>(raw);
    if (!ctx || !ctx->lock) {
      return false;
    }
    // Briefly release IO so inject can drain + SendInput between TLS records.
    ctx->lock->unlock();
    if (g_inject_active.load() || (g_ptr_buttons.load() & 1) != 0) {
      Sleep(0);
    }
    ctx->lock->lock();
    return true;
  };

  auto video_out_thread_main = [&]() {
    logf("video-out thread start");
    VideoWriteYieldCtx yield_ctx{};
    yield_ctx.shared = &shared;
    while (!shared.video_out_stop.load() &&
           !(stop_requested && stop_requested->load())) {
      std::vector<uint8_t> msg;
      {
        std::lock_guard<std::mutex> lock(shared.outq_mu);
        if (shared.outq.empty()) {
          // fall through after unlock
        } else {
          OutBatch& batch = shared.outq.front();
          if (batch.msgs.empty()) {
            shared.outq.pop_front();
          } else {
            msg = std::move(batch.msgs.front());
            batch.msgs.erase(batch.msgs.begin());
            if (batch.msgs.empty()) {
              shared.outq.pop_front();
            }
          }
        }
      }
      if (msg.empty()) {
        Sleep(1);
        continue;
      }

      shared.video_writing.store(true);
      // Prefer inject while dragging — don't grab IO until inject finishes a burst.
      while ((g_ptr_buttons.load() & 1) != 0 && g_inject_active.load()) {
        Sleep(0);
      }
      std::unique_lock<std::mutex> lock(shared.io_mu);
      yield_ctx.lock = &lock;
      bool any_authed = false;
      bool any_sent = false;
      bool all_blocked = true;
      for (ClientConn& c : shared.clients) {
        if (!c.authed || c.dead) {
          continue;
        }
        any_authed = true;
        if (!socket_writable(tls::tls_get_socket(c.tls), 0)) {
          // During drag, forcing keyframes just floods Win7 with full-desk JPEG.
          if ((g_ptr_buttons.load() & 1) == 0) {
            c.need_keyframe = true;
            g_any_need_keyframe.store(true);
          }
          continue;
        }
        all_blocked = false;
        const bool big = msg.size() >= 16u * 1024u;
        const bool ok =
            big ? mux_write_yield(c.tls, kChannelVideo, msg.data(),
                                  static_cast<uint32_t>(msg.size()), video_write_yield,
                                  &yield_ctx)
                : mux_write(c.tls, kChannelVideo, msg.data(),
                            static_cast<uint32_t>(msg.size()));
        if (!ok) {
          close_client(&c, mutex);
          continue;
        }
        any_sent = true;
      }
      yield_ctx.lock = nullptr;
      lock.unlock();
      shared.video_writing.store(false);
      if (!any_authed) {
        std::lock_guard<std::mutex> qlock(shared.outq_mu);
        shared.outq.clear();
        continue;
      }
      if (!any_sent && all_blocked) {
        std::lock_guard<std::mutex> qlock(shared.outq_mu);
        shared.outq.push_front(OutBatch{});
        shared.outq.front().msgs.push_back(std::move(msg));
        Sleep(1);
      }
      (void)any_sent;
    }
    logf("video-out thread end");
  };

  auto encode_thread_main = [&]() {
    try {
    // WIC / MF / DXGI COM objects are created on this thread when capture begins.
    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_uninit = (co_hr == S_OK);
    if (FAILED(co_hr) && co_hr != RPC_E_CHANGED_MODE) {
      logf("encode thread: CoInitializeEx failed hr=0x%08lx", static_cast<unsigned long>(co_hr));
    }

    logf("encode thread start modern_lossy=%d", modern_lossy ? 1 : 0);

    // Construct capture lazily — keeps idle host thin (crash was right after thread start).
    std::unique_ptr<SessionCapture> cap;
    bool cap_begun = false;
    ShakeGuard shake_guard;
    std::vector<uint8_t> frame;
    std::vector<uint8_t> prev;
    std::vector<uint8_t> raw_buf;
    std::vector<uint8_t> send_buf;
    std::vector<DirtyRect> dirties;
    std::vector<CaptureDirty> mirror_dirties;
    std::vector<CaptureMove> mirror_moves;
    std::vector<CopyRect> copies;
    uint32_t frame_id = 0;
    uint32_t sent_rects = 0;
    uint32_t ticks_sent = 0;
    bool have_prev = false;
    DWORD last_send_ms = 0;
    DWORD last_stat_ms = GetTickCount();
    DWORD last_attach_scrub_ms = GetTickCount();
    constexpr DWORD kStatIntervalMs = 1000;
    constexpr DWORD kAttachScrubMs = 2000;
    bool logged_rec_fail = false;

    while (!shared.encode_stop.load() &&
           !(stop_requested && stop_requested->load())) {
      if (!shared.session_active.load()) {
        if (cap_begun) {
          session_recorder_reset();
          logged_rec_fail = false;
          logf("session-record: stopped (artifacts under debug\\)");
          if (cap) {
            cap->end();
          }
          cap_begun = false;
          cap.reset();
          restore_host_cursor_after_session();
          g_video_ptr_have.store(false);
          aero_shake_restore(&shake_guard);
          have_prev = false;
          prev.clear();
          media_log_set_mirror_stderr(true);
          logf("all viewers gone - capture stopped");
        }
        Sleep(5);
        continue;
      }

      if (!cap) {
        cap = std::make_unique<SessionCapture>(capture_mode);
      }

      if (!cap_begun) {
        if (!cap->begin()) {
          logf("capture begin failed");
          Sleep(50);
          continue;
        }
        cap_begun = true;
        media_log_set_mirror_stderr(false);
        aero_shake_disable(&shake_guard);
        hide_host_cursor_for_session();
        if (cap->using_mirror()) {
          logf("capture started mirror device=%s", cap->mirror_device());
        } else if (cap->using_dxgi()) {
          logf("capture started dxgi");
        } else {
          logf("capture started gdi");
        }
        g_input_pointer = 0;
        g_input_key = 0;
        g_ptr_have_prev.store(false);
        g_ptr_buttons_prev.store(0);
        g_validate_frames.store(0);
        g_copyrects_sent.store(0);
        g_video_ptr_have.store(false);
        g_any_need_keyframe.store(true);
      }

      const DWORD loop_now = GetTickCount();
      if (cap->using_mirror() && loop_now - last_attach_scrub_ms >= kAttachScrubMs) {
        last_attach_scrub_ms = loop_now;
        rdm_scrub_foreign_attach_registry();
      }

      const DWORD now = GetTickCount();
      if (last_send_ms != 0 && now - last_send_ms < 8u) {
        Sleep(1);
        continue;
      }

      bool outq_full = false;
      {
        std::lock_guard<std::mutex> lock(shared.outq_mu);
        const size_t max_b = (g_ptr_buttons.load() & 1) ? 1u : kMaxBatches;
        if (shared.outq.size() >= max_b) {
          shared.dropped_ticks.fetch_add(1);
          shared.dropped_interval.fetch_add(1);
          outq_full = true;
        }
      }

      const bool any_kf = g_any_need_keyframe.load();
      const bool drag_mode = (g_ptr_buttons.load() & 1) != 0;
      int validate = g_validate_frames.load();
      // DXGI drag: never force_full_pixels — that clears MoveRects (lab copy=0).
      const bool force_validate =
          cap->provides_dirties() && validate > 0 && !(cap->using_dxgi() && drag_mode);
      const bool force_full = !have_prev || (any_kf && !drag_mode);

      int w = 0;
      int h = 0;
      const DWORD cap0 = GetTickCount();
      mirror_dirties.clear();
      mirror_moves.clear();
      if (!cap->capture(&frame, &w, &h, &mirror_dirties, &mirror_moves, force_validate)) {
        Sleep(5);
        continue;
      }
      const DWORD cap_ms = GetTickCount() - cap0;

      // Always pump capture (esp. DXGI) even when outq is full — skipping AcquireNextFrame
      // coalesces MoveRects into a full DirtyRect (lab: copy=0 + 1MB zlib).
      if (outq_full) {
        const size_t frame_bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
        if (frame.size() == frame_bytes && !frame.empty()) {
          if (!have_prev || prev.size() != frame_bytes) {
            prev.assign(frame_bytes, 0);
          }
          prev = frame;
          have_prev = true;
        }
        Sleep(1);
        continue;
      }

      if (!session_recorder_active() && session_recorder_enabled() && w > 0 && h > 0) {
        SessionRecorderInfo ri{};
        if (session_recorder_begin(w, h, &ri)) {
          logf("session-record: started version=%s %ux%u->%ux%u %s", ROAD_DESK_VERSION_STRING,
               static_cast<unsigned>(w), static_cast<unsigned>(h),
               static_cast<unsigned>(w & ~1), static_cast<unsigned>(h & ~1), ri.video_path.c_str());
          logged_rec_fail = false;
        } else if (!logged_rec_fail) {
          logged_rec_fail = true;
          logf("session-record: unavailable this session (MF/AVI) version=%s",
               ROAD_DESK_VERSION_STRING);
        }
      }
      if (session_recorder_active() && !frame.empty()) {
        // Win7: skip AVI encode while dragging — competes with video JPEG on a weak VM.
        if (!(!modern_lossy && drag_mode)) {
          session_recorder_push_bgra(frame.data(), w, h);
        }
      }

      const size_t frame_bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
      if (frame.size() != frame_bytes || frame.empty()) {
        Sleep(5);
        continue;
      }
      if (have_prev && prev.size() != frame_bytes) {
        have_prev = false;
      }
      if (!have_prev) {
        prev.assign(frame_bytes, 0);
      }

      copies.clear();
      dirties.clear();

      // DXGI MoveRects always (including drag) — display-path CopyRect like VNC.
      if (cap->using_dxgi() && !force_validate && !force_full) {
        for (const CaptureMove& m : mirror_moves) {
          if (rect_in_desk(w, h, m.sx, m.sy, m.w, m.h) &&
              rect_in_desk(w, h, m.dx, m.dy, m.w, m.h)) {
            copies.push_back(CopyRect{m.sx, m.sy, m.dx, m.dy, m.w, m.h});
          }
        }
      }

      if (cap->provides_dirties() && !force_validate && !force_full) {
        for (const CaptureDirty& d : mirror_dirties) {
          dirties.push_back(DirtyRect{d.x, d.y, d.w, d.h});
        }
      } else {
        collect_dirty_rects(frame.data(), have_prev && !force_full ? prev.data() : nullptr, w, h,
                            force_full, &dirties);
      }

      // Win7/Mirror: no pixel CopyRect (flooded IO or capped to useless). Dirties only.

      if (force_validate && validate > 0) {
        g_validate_frames.store(validate - 1);
      }

      if (dirties.empty() && copies.empty()) {
        continue;
      }

      const unsigned before_collapse = static_cast<unsigned>(dirties.size() + copies.size());
      // Mirror drag: collapse to AABB for JPEG strips. DXGI with CopyRects: do not
      // union dirties into a near-full zlib (that wiped MoveRect gains).
      collapse_dirties_for_send(&dirties, w, h, drag_mode && copies.empty());

      const bool full_keyframe =
          dirties.size() == 1 && dirties[0].x == 0 && dirties[0].y == 0 && dirties[0].w == w &&
          dirties[0].h == h;
      if (!full_keyframe) {
        // Legacy drag: keep one AABB JPEG — splitting into many WIC encodes starved Win7 (0.4.2).
        if (!(!modern_lossy && drag_mode)) {
          split_rects_for_input_slices(&dirties, 256);
        }
      }

      bool batch_ok = true;
      size_t raw_this = 0;
      size_t wire_this = 0;
      DWORD enc_this = 0;
      unsigned zlib_this = 0;
      unsigned jpeg_this = 0;
      unsigned h264_this = 0;
      unsigned copy_this = 0;
      const unsigned moves_in = static_cast<unsigned>(mirror_moves.size());
      const unsigned dirties_in = static_cast<unsigned>(mirror_dirties.size());
      OutBatch tick_batch;

      for (const CopyRect& cr : copies) {
        std::vector<uint8_t> body(kCopyRectPayloadSize);
        body[0] = kVideoCopyRect;
        write_u32_le(body.data() + 1, frame_id);
        write_u16_le(body.data() + 5, static_cast<uint16_t>(cr.dx));
        write_u16_le(body.data() + 7, static_cast<uint16_t>(cr.dy));
        write_u16_le(body.data() + 9, static_cast<uint16_t>(cr.w));
        write_u16_le(body.data() + 11, static_cast<uint16_t>(cr.h));
        write_u16_le(body.data() + 13, static_cast<uint16_t>(cr.sx));
        write_u16_le(body.data() + 15, static_cast<uint16_t>(cr.sy));
        tick_batch.msgs.push_back(std::move(body));
        g_copyrects_sent.fetch_add(1);
        ++copy_this;
        ++sent_rects;
        ++frame_id;
        wire_this += kCopyRectPayloadSize;
      }

      const int jpeg_q = drag_mode ? 40 : 78;

      for (const DirtyRect& r : dirties) {
        if (!rect_in_desk(w, h, r.x, r.y, r.w, r.h)) {
          have_prev = false;
          batch_ok = false;
          break;
        }
        const DWORD enc0 = GetTickCount();
        EncodeRectStats enc{};
        const size_t area = static_cast<size_t>(r.w) * static_cast<size_t>(r.h);
        const size_t desk_area = static_cast<size_t>(w) * static_cast<size_t>(h);
        // After first KF: never ship full-desk zlib (~1MB) — it holds io_mu and starves inject.
        const bool huge = area * 2u > desk_area;
        const bool allow_lossy = !full_keyframe || drag_mode || (have_prev && huge);
        // Drag: JPEG only — H264 encode latency makes Viewer cursor lead the window.
        const bool try_h264 =
            modern_lossy && allow_lossy && !drag_mode && area >= static_cast<size_t>(96 * 96);
        const bool try_jpeg = allow_lossy && area >= static_cast<size_t>(96 * 96);
        const int q = (drag_mode && huge) ? 32 : jpeg_q;
        bool ok = false;
        if (try_h264) {
          ok = encode_h264_rect(frame_id, w, h, r.x, r.y, r.w, r.h, frame.data(), 72, &raw_buf,
                                &send_buf, &enc);
        }
        if (!ok && try_jpeg) {
          ok = encode_jpeg_rect(frame_id, w, h, r.x, r.y, r.w, r.h, frame.data(), q, &raw_buf,
                                &send_buf, &enc);
        }
        if (!ok && !(have_prev && huge)) {
          ok = encode_video_rect(frame_id, w, h, r.x, r.y, r.w, r.h, frame.data(), &raw_buf,
                                 &send_buf, &enc);
        }
        if (!ok) {
          // Drop this rect rather than block the pipe with megabyte zlib.
          shared.dropped_ticks.fetch_add(1);
          shared.dropped_interval.fetch_add(1);
          continue;
        }
        enc_this += GetTickCount() - enc0;
        raw_this += enc.raw_bytes;
        wire_this += enc.wire_bytes;
        if (enc.codec == kVideoZlibBgra) {
          ++zlib_this;
        } else if (enc.codec == kVideoJpeg) {
          ++jpeg_this;
        } else if (enc.codec == kVideoH264) {
          ++h264_this;
        }
        tick_batch.msgs.push_back(std::vector<uint8_t>(send_buf.begin(), send_buf.end()));
        ++sent_rects;
        ++frame_id;
      }

      if (!batch_ok) {
        shared.dropped_ticks.fetch_add(1);
        shared.dropped_interval.fetch_add(1);
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(shared.outq_mu);
        const size_t max_b = (g_ptr_buttons.load() & 1) ? 1u : kMaxBatches;
        while (shared.outq.size() >= max_b) {
          shared.outq.pop_front();
          shared.dropped_ticks.fetch_add(1);
          shared.dropped_interval.fetch_add(1);
        }
        if (!tick_batch.msgs.empty()) {
          shared.outq.push_back(std::move(tick_batch));
        }
      }

      g_video_ptr_x.store(g_ptr_x.load());
      g_video_ptr_y.store(g_ptr_y.load());
      g_video_ptr_have.store(true);
      if (full_keyframe || any_kf) {
        g_any_need_keyframe.store(false);
        // Per-client flags were never cleared (lab Win7: force_full every tick → 220KB JPEG storm).
        g_clear_client_keyframes.store(true);
      }

      for (const CopyRect& cr : copies) {
        copy_rect_to_prev(prev.data(), frame.data(), w, h, DirtyRect{cr.dx, cr.dy, cr.w, cr.h});
      }
      for (const DirtyRect& r : dirties) {
        copy_rect_to_prev(prev.data(), frame.data(), w, h, r);
      }
      have_prev = true;
      last_send_ms = GetTickCount();
      ++ticks_sent;

      if (ticks_sent == 1 || last_send_ms - last_stat_ms >= kStatIntervalMs) {
        last_stat_ms = last_send_ms;
        size_t outq_n = 0;
        {
          std::lock_guard<std::mutex> lock(shared.outq_mu);
          outq_n = shared.outq.size();
        }
        const uint32_t di = shared.dropped_interval.exchange(0);
        logf("tick=%u batch=%u(from %u) copy=%u moves_in=%u dirty_in=%u wire=%u zlib=%u jpeg=%u "
             "h264=%u cap_ms=%u outq=%u drop=%u drop_total=%u",
             ticks_sent, static_cast<unsigned>(dirties.size() + copies.size()), before_collapse,
             copy_this, moves_in, dirties_in, static_cast<unsigned>(wire_this), zlib_this,
             jpeg_this, h264_this, cap_ms, static_cast<unsigned>(outq_n), di,
             shared.dropped_ticks.load());
      }
    }

    if (cap_begun) {
      session_recorder_reset();
      if (cap) {
        cap->end();
      }
      cap.reset();
      restore_host_cursor_after_session();
      aero_shake_restore(&shake_guard);
    }
    h264_codec_shutdown();
    media_log_set_mirror_stderr(true);
    logf("encode thread end ticks=%u rects=%u", ticks_sent, sent_rects);
    if (co_uninit) {
      CoUninitialize();
    }
    } catch (const std::exception& ex) {
      restore_host_cursor_after_session();
      logf("encode thread FATAL exception: %s", ex.what());
    } catch (...) {
      restore_host_cursor_after_session();
      logf("encode thread FATAL unknown exception");
    }
  };

  std::thread encode_thr(encode_thread_main);
  std::thread video_out_thr(video_out_thread_main);
  logf("shared-control serve (max_clients=%u modern_lossy=%d inject+video-out+encode)",
       static_cast<unsigned>(kMaxClients), modern_lossy ? 1 : 0);

  DWORD last_cursor_ms = 0;
  DWORD last_alive_ms = GetTickCount();
  bool logged_io_alive = false;

  while (!(stop_requested && stop_requested->load()) && listen_sock != INVALID_SOCKET) {
    const bool dragging = (g_ptr_buttons.load() & 1) != 0;

    g_inject_active.store(true);
    {
      std::lock_guard<std::mutex> lock(shared.io_mu);
      drain_all_clients(&shared.clients, psk, mutex);
      if (!dragging && !shared.video_writing.load()) {
        fanout_host_clipboard(&shared.clients, &shared_xfer_id, psk, mutex);
      }
    }
    g_inject_active.store(false);

    unsigned authed = 0;
    {
      std::lock_guard<std::mutex> lock(shared.io_mu);
      authed = count_authed(shared.clients);
    }
    shared.session_active.store(authed > 0);
    if (authed == 0) {
      release_modifiers();
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listen_sock, &rfds);
    {
      std::lock_guard<std::mutex> lock(shared.io_mu);
      for (const ClientConn& c : shared.clients) {
        if (c.tls && !c.dead) {
          const SOCKET s = tls::tls_get_socket(c.tls);
          if (s != INVALID_SOCKET) {
            FD_SET(s, &rfds);
          }
        }
      }
    }
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = (authed == 0) ? 50000 : (dragging ? 0 : 2000);
    const int sel = select(0, &rfds, nullptr, nullptr, &tv);
    if (sel < 0) {
      const int err = WSAGetLastError();
      if (stop_requested && stop_requested->load()) {
        break;
      }
      logf("select failed (%d)", err);
      break;
    }

    if (sel > 0 && FD_ISSET(listen_sock, &rfds)) {
      SOCKET tcp = tls::tcp_accept(listen_sock);
      if (tcp != INVALID_SOCKET) {
        std::lock_guard<std::mutex> lock(shared.io_mu);
        if (shared.clients.size() >= kMaxClients) {
          logf("reject: max clients (%u)", static_cast<unsigned>(kMaxClients));
          closesocket(tcp);
        } else if (!creds) {
          closesocket(tcp);
        } else {
          tls::TlsSession* tls_sess = tls::server_handshake(tcp, *creds);
          if (!tls_sess) {
            logf("TLS handshake failed");
          } else {
            ClientConn c;
            c.tls = tls_sess;
            shared.clients.push_back(std::move(c));
            logf("TLS up clients=%u/%u - awaiting auth",
                 static_cast<unsigned>(shared.clients.size()),
                 static_cast<unsigned>(kMaxClients));
          }
        }
      }
    }

    g_inject_active.store(true);
    {
      std::lock_guard<std::mutex> lock(shared.io_mu);
      drain_all_clients(&shared.clients, psk, mutex);
    }
    g_inject_active.store(false);

    const DWORD loop_now = GetTickCount();
    if (!dragging && !shared.video_writing.load() && loop_now - last_cursor_ms >= 50) {
      last_cursor_ms = loop_now;
      CursorShape shape;
      shape.hidden = true;
      shape.hash = 1;
      std::lock_guard<std::mutex> lock(shared.io_mu);
      for (ClientConn& c : shared.clients) {
        if (!c.authed || c.dead) {
          continue;
        }
        if (!send_cursor_if_changed(c.tls, shape, &c.cursor_hash_sent)) {
          close_client(&c, mutex);
        }
      }
      shared.clients.erase(std::remove_if(shared.clients.begin(), shared.clients.end(),
                                          [](const ClientConn& c) { return c.dead || !c.tls; }),
                           shared.clients.end());
    }

    g_inject_active.store(true);
    {
      std::lock_guard<std::mutex> lock(shared.io_mu);
      drain_all_clients(&shared.clients, psk, mutex);
      shared.clients.erase(std::remove_if(shared.clients.begin(), shared.clients.end(),
                                          [](const ClientConn& c) { return c.dead || !c.tls; }),
                           shared.clients.end());
      if (g_clear_client_keyframes.exchange(false)) {
        for (ClientConn& c : shared.clients) {
          c.need_keyframe = false;
        }
      }
      bool any_kf = false;
      for (ClientConn& c : shared.clients) {
        if (c.authed && !c.dead && c.need_keyframe) {
          any_kf = true;
        }
      }
      if (any_kf) {
        g_any_need_keyframe.store(true);
      }
    }
    g_inject_active.store(false);

    if (!logged_io_alive && GetTickCount() - last_alive_ms >= 1000) {
      logged_io_alive = true;
      logf("inject thread alive (video-out+encode concurrent)");
    }
  }

  shared.encode_stop.store(true);
  shared.video_out_stop.store(true);
  shared.session_active.store(false);
  if (encode_thr.joinable()) {
    encode_thr.join();
  }
  if (video_out_thr.joinable()) {
    video_out_thr.join();
  }

  {
    std::lock_guard<std::mutex> lock(shared.io_mu);
    for (ClientConn& c : shared.clients) {
      close_client(&c, mutex);
    }
    shared.clients.clear();
  }
  {
    std::lock_guard<std::mutex> lock(shared.outq_mu);
    shared.outq.clear();
  }
  release_modifiers();
  logf("shared-control serve end");
}


}  // namespace

struct MediaPlane::Impl {
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> running{false};
  std::atomic<int> bound_port{0};
  bool wsa_started = false;
  bool log_opened = false;
  SOCKET listen_sock = INVALID_SOCKET;
  tls::HostCredentials tls_creds{};
  std::string password;
  std::string fingerprint;
  session::SessionMutex* session_mutex = nullptr;
  road_desk::replace::CaptureMode capture_mode = road_desk::replace::CaptureMode::Auto;
  road_desk::replace::CaptureStrategy capture_strategy = road_desk::replace::CaptureStrategy::Legacy;

  void cleanup_listen() {
    if (listen_sock != INVALID_SOCKET) {
      closesocket(listen_sock);
      listen_sock = INVALID_SOCKET;
    }
    tls::free_host_credentials(&tls_creds);
    fingerprint.clear();
    bound_port = 0;
  }
};

MediaPlane::MediaPlane() : impl_(new Impl) {}

MediaPlane::~MediaPlane() {
  request_stop();
  if (impl_) {
    impl_->cleanup_listen();
    if (impl_->wsa_started) {
      WSACleanup();
      impl_->wsa_started = false;
    }
    if (impl_->log_opened) {
      media_log_close();
      impl_->log_opened = false;
    }
    impl_->running = false;
  }
  delete impl_;
  impl_ = nullptr;
}

bool MediaPlane::listen(const MediaPlaneConfig& config) {
  if (!impl_ || impl_->running.load() || impl_->listen_sock != INVALID_SOCKET) {
    return false;
  }

  SetProcessDPIAware();
  // Prefer host-agent.log (opened by agent main). Open it here if standalone.
  const bool already = media_log_is_open();
  media_log_open("host-agent.log");
  impl_->log_opened = !already;
  logf("boot media-plane mux host");

  // Heal invisible pointer left by a prior host crash (SetSystemCursor blank).
  restore_host_cursor_after_session();

  road_desk::replace::release_modifiers();

  BOOL elevated = FALSE;
  {
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
      TOKEN_ELEVATION elev{};
      DWORD got = 0;
      if (GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &got)) {
        elevated = elev.TokenIsElevated ? TRUE : FALSE;
      }
      CloseHandle(tok);
    }
    if (!elevated) {
      logf("WARN: not elevated — cannot scrub HKLM Attach.ToDesktop; "
           "opening Device Manager will freeze mouse/keyboard (need Administrator for mirror)");
    } else {
      logf("boot: elevated (Administrator)");
    }
  }

  auto close_log_if_owned = [&]() {
    if (impl_->log_opened) {
      media_log_close();
      impl_->log_opened = false;
    }
  };

  if (!config.require_tls) {
    logf("require_tls=false rejected — mux host has no plaintext path");
    close_log_if_owned();
    return false;
  }

  if (!impl_->wsa_started) {
    if (!wsa_init()) {
      logf("WSAStartup failed");
      close_log_if_owned();
      return false;
    }
    impl_->wsa_started = true;
  }

  impl_->password = config.password;
  impl_->session_mutex = config.session_mutex;
  impl_->stop_requested = false;
  {
    char* e = nullptr;
    size_t n = 0;
    const char* env_cap = nullptr;
    if (_dupenv_s(&e, &n, "ROAD_DESK_CAPTURE") == 0 && e) {
      env_cap = e;
    }
    const road_desk::replace::CaptureResolve resolved = road_desk::replace::resolve_capture(env_cap);
    if (e) {
      free(e);
    }
    impl_->capture_mode = resolved.effective;
    impl_->capture_strategy = resolved.strategy;
    if (!resolved.os.ok) {
      logf("WARN os_version_failed strategy=legacy (conservative)");
    }
    if (resolved.note && resolved.note[0]) {
      logf("os=%u.%u.%u strategy=%s capture=%s resolved=%s (%s)", resolved.os.major,
           resolved.os.minor, resolved.os.build, road_desk::replace::capture_strategy_name(resolved.strategy),
           road_desk::replace::capture_mode_name(resolved.requested),
           road_desk::replace::capture_mode_name(resolved.effective), resolved.note);
    } else {
      logf("os=%u.%u.%u strategy=%s capture=%s resolved=%s", resolved.os.major, resolved.os.minor,
           resolved.os.build, road_desk::replace::capture_strategy_name(resolved.strategy),
           road_desk::replace::capture_mode_name(resolved.requested),
           road_desk::replace::capture_mode_name(resolved.effective));
    }
  }

  // Only scrub Mirror Attach when we may actually attach (legacy auto / force mirror).
  if (impl_->capture_mode == road_desk::replace::CaptureMode::Auto ||
      impl_->capture_mode == road_desk::replace::CaptureMode::Mirror) {
    if (rdm_force_detach()) {
      if (elevated) {
        logf("mirror force-detach ok (Attach.ToDesktop cleared)");
      } else {
        logf("mirror force-detach attempted (not elevated — HKLM scrub may have failed)");
      }
    }
  }

  if (!session::authenticate_psk(impl_->password, impl_->password)) {
    logf("PSK required - empty password rejected");
    if (impl_->wsa_started) {
      WSACleanup();
      impl_->wsa_started = false;
    }
    close_log_if_owned();
    return false;
  }

  if (!tls::create_self_signed_credentials(&impl_->tls_creds)) {
    logf("create_self_signed_credentials failed");
    if (impl_->wsa_started) {
      WSACleanup();
      impl_->wsa_started = false;
    }
    close_log_if_owned();
    return false;
  }
  impl_->fingerprint = impl_->tls_creds.fingerprint_sha256_hex;
  logf("TLS fingerprint SHA-256: %s", impl_->fingerprint.c_str());

  impl_->listen_sock = tls::tcp_listen(config.listen_port);
  if (impl_->listen_sock == INVALID_SOCKET) {
    const int wsa = WSAGetLastError();
    logf("listen failed on %d (WSA=%d)%s", config.listen_port, wsa,
         wsa == WSAEADDRINUSE ? " — port already in use (old host-agent / VNC?)" : "");
    tls::free_host_credentials(&impl_->tls_creds);
    impl_->fingerprint.clear();
    if (impl_->wsa_started) {
      WSACleanup();
      impl_->wsa_started = false;
    }
    close_log_if_owned();
    return false;
  }

  impl_->bound_port = config.listen_port;
  impl_->running = true;
  logf("listening TCP+TLS port %d (private mux, not RFB) version=%s built=%s %s",
       config.listen_port, ROAD_DESK_VERSION_STRING, ROAD_DESK_BUILD_DATE, ROAD_DESK_BUILD_TIME);
  return true;
}

void MediaPlane::serve() {
  if (!impl_ || impl_->listen_sock == INVALID_SOCKET) {
    return;
  }

  logf("serve: shared-control multi-viewer");
  serve_shared(impl_->listen_sock, impl_->password, impl_->capture_mode, impl_->capture_strategy,
               &impl_->stop_requested, impl_->session_mutex, &impl_->tls_creds);

  road_desk::replace::release_modifiers();
  impl_->cleanup_listen();
  impl_->running = false;
  logf("serve end");
}

void MediaPlane::request_stop() {
  if (!impl_) {
    return;
  }
  impl_->stop_requested = true;
  if (impl_->listen_sock != INVALID_SOCKET) {
    closesocket(impl_->listen_sock);
    impl_->listen_sock = INVALID_SOCKET;
  }
}

bool MediaPlane::running() const {
  return impl_ && impl_->running.load();
}

int MediaPlane::bound_port() const {
  return impl_ ? impl_->bound_port.load() : 0;
}

std::string MediaPlane::tls_fingerprint_sha256() const {
  if (!impl_) {
    return {};
  }
  return impl_->fingerprint;
}

}  // namespace road_desk::media
