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
#include "proc_stats.h"
#include "product_version.h"
#include "session_mutex.h"
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
#include <ws2tcpip.h>
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

// Same block-diff as collect_dirty_rects but scoped to (rx,ry,rw,rh). Used by the
// GDI drag region path: only the moving window + wake are scanned each tick.
void collect_dirty_rects_region(const uint8_t* cur, const uint8_t* prev, int desk_w, int rx, int ry,
                                int rw, int rh, std::vector<DirtyRect>* out) {
  out->clear();
  if (rw <= 0 || rh <= 0) {
    return;
  }
  const int xend = rx + rw;
  const int yend = ry + rh;
  constexpr int kBlock = 32;
  for (int y = ry; y < yend; y += kBlock) {
    const int yh = (y + kBlock < yend) ? (y + kBlock) : yend;
    int run_x0 = -1;
    int run_x1 = -1;
    for (int x = rx; x < xend; x += kBlock) {
      const int xw = (x + kBlock < xend) ? (x + kBlock) : xend;
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
  if (rects->size() <= 1) {
    return;
  }
  // Drag used to be bbox-merged unconditionally, but at 20-60Hz a moving window
  // diffs into thin edge strips + exposed background. Bboxing them into one
  // giant AABB re-encoded the whole window every tick (win7 lab: 16-62ms JPEG
  // per drag tick). Keep the strips unless there is a fragment storm (>24 rects)
  // or the dirty area covers most of the desktop.
  size_t area = 0;
  for (const DirtyRect& r : *rects) {
    area += static_cast<size_t>(r.w) * static_cast<size_t>(r.h);
  }
  const size_t screen = static_cast<size_t>(desk_w) * static_cast<size_t>(desk_h);
  const bool mostly_dirty = area * 2u > screen;
  const bool fragment_storm = rects->size() > 24u;
  if (!mostly_dirty && !fragment_storm) {
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
  if (x1 <= x0 || y1 <= y0) {
    return;
  }
  // Non-drag + mostly-dirty used to force a full-desk keyframe. Explorer select can
  // dirty a large pane; that KF then went out as JPEG and flashed the whole Viewer.
  // Keep refined strips so only real pixel changes are encoded.
  if (mostly_dirty && !drag_mode) {
    return;
  }
  rects->assign(1, {x0, y0, x1 - x0, y1 - y0});
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

struct PendingInput {
  uint8_t kind = 0;
  uint8_t buttons = 0;
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t vk = 0;
  bool down = false;
  bool extended = false;
};
// Queued input awaiting injection (see flush_pending_input). Touched only by
// the inject thread: appended under io_mu while draining, drained without it.
std::vector<PendingInput> g_pending_input;

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
      // One validation tick per LMB edge (was 2): each forces a full Mirror blit
      // + whole-desk diff on Win7, doubling the per-click CPU spike for no extra
      // correctness (drag ticks full-diff anyway).
      g_validate_frames.store(1);
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
    PendingInput pi{};
    pi.kind = kInputPointer;
    pi.buttons = static_cast<uint8_t>(buttons);
    pi.x = static_cast<uint16_t>(x);
    pi.y = static_cast<uint16_t>(y);
    g_pending_input.push_back(pi);
    return;
  }
  if (payload[0] == kInputKey && payload.size() >= kInputKeySize) {
    ++g_input_key;
    const bool down = (payload[1] & kInputKeyFlagDown) != 0;
    const bool extended = (payload[1] & kInputKeyFlagExtended) != 0;
    PendingInput pi{};
    pi.kind = kInputKey;
    pi.vk = read_u16_le(payload.data() + 2);
    pi.down = down;
    pi.extended = extended;
    g_pending_input.push_back(pi);
  }
}

// Input events are read under io_mu (TLS) but injected after the lock is
// released — SendInput on a VM can take milliseconds per event, and holding
// io_mu during it starves video-out (drag window/mouse desync on Win10).
void flush_pending_input() {
  using namespace road_desk::replace;
  if (g_pending_input.empty()) {
    return;
  }
  std::vector<PendingInput> q;
  q.swap(g_pending_input);
  for (const PendingInput& p : q) {
    if (p.kind == kInputPointer) {
      inject_pointer(p.buttons, p.x, p.y);
    } else if (p.kind == kInputKey) {
      inject_vk(p.vk, p.down, p.extended);
    }
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

bool drain_incoming(tls::TlsSession* tls, HostClipSession* clip, bool allow_control) {
  using namespace road_desk::replace;
  const SOCKET sock = tls::tls_get_socket(tls);
  // Bound how many input events are injected per io_mu hold — a fast mouse burst
  // otherwise keeps the inject thread holding the lock and starves video-out.
  unsigned input_budget = 64;
  while (socket_readable(sock, 0)) {
    uint8_t ch = 0;
    std::vector<uint8_t> payload;
    if (!mux_read(tls, &ch, &payload)) {
      return false;
    }
    if (ch == kChannelInput) {
      if (allow_control) {
        handle_input(payload);
        if (--input_budget == 0) {
          return true;  // leave the rest for the next drain pass (inject spins)
        }
      }
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
      if (!allow_control) {
        // Watchers may still send; drop mutate (A5a). Keep reading to stay in sync.
        continue;
      }
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
      if (!allow_control) {
        continue;
      }
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
  return drain_incoming(tls, clip, true);
}

constexpr size_t kMaxClients = 8;

struct ClientConn {
  tls::TlsSession* tls = nullptr;
  bool authed = false;
  bool can_control = false;  // A5a: at most one true among authed clients
  bool need_keyframe = false;
  bool dead = false;
  bool audit_opened = false;
  uint32_t cursor_hash_sent = 0;
  std::string session_id;
  std::string viewer_ip;
  HostClipSession clip;
};

std::string socket_peer_ip(SOCKET s) {
  sockaddr_storage ss{};
  int len = sizeof(ss);
  if (s == INVALID_SOCKET || getpeername(s, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
    return {};
  }
  char buf[INET6_ADDRSTRLEN] = {};
  if (ss.ss_family == AF_INET) {
    auto* in = reinterpret_cast<sockaddr_in*>(&ss);
    if (InetNtopA(AF_INET, &in->sin_addr, buf, sizeof(buf))) {
      return buf;
    }
  } else if (ss.ss_family == AF_INET6) {
    auto* in6 = reinterpret_cast<sockaddr_in6*>(&ss);
    if (InetNtopA(AF_INET6, &in6->sin6_addr, buf, sizeof(buf))) {
      return buf;
    }
  }
  return {};
}

void emit_audit(MediaPlaneConfig::AuditFn fn, void* user, const char* phase, const char* result,
                const char* disconnect_reason, const std::string& session_id,
                const std::string& viewer_ip, const char* mode = nullptr) {
  if (!fn) {
    return;
  }
  MediaPlaneConfig::AuditEvent ev;
  ev.phase = phase;
  ev.result = result;
  ev.disconnect_reason = disconnect_reason;
  ev.session_id = session_id.empty() ? nullptr : session_id.c_str();
  ev.viewer_ip = viewer_ip.empty() ? nullptr : viewer_ip.c_str();
  ev.mode = mode;
  fn(user, &ev);
}

void close_client(ClientConn* c, session::SessionMutex* mutex, MediaPlaneConfig::AuditFn audit_fn,
                  void* audit_user) {
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
  if (c->audit_opened) {
    emit_audit(audit_fn, audit_user, "closed", "ok", "transport_lost", c->session_id, c->viewer_ip,
               c->can_control ? "control" : "view_only");
    c->audit_opened = false;
  }
  c->authed = false;
  c->can_control = false;
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

bool has_controller(const std::vector<ClientConn>& clients, const ClientConn* except) {
  for (const ClientConn& o : clients) {
    if (except && &o == except) {
      continue;
    }
    if (o.authed && !o.dead && o.can_control) {
      return true;
    }
  }
  return false;
}

// When the controller leaves, promote the earliest remaining authed viewer and notify it.
void ensure_one_controller(std::vector<ClientConn>* clients, MediaPlaneConfig::AuditFn audit_fn,
                           void* audit_user) {
  using namespace road_desk::replace;
  if (!clients || has_controller(*clients, nullptr)) {
    return;
  }
  for (ClientConn& c : *clients) {
    if (!c.authed || c.dead || !c.tls) {
      continue;
    }
    c.can_control = true;
    if (!control_send_session_role(c.tls, kSessionRoleControl)) {
      logf("promote: SessionRole send failed peer=%s",
           c.viewer_ip.empty() ? "-" : c.viewer_ip.c_str());
    } else {
      logf("promoted to control peer=%s session=%s",
           c.viewer_ip.empty() ? "-" : c.viewer_ip.c_str(),
           c.session_id.empty() ? "-" : c.session_id.c_str());
    }
    emit_audit(audit_fn, audit_user, "flag", "ok", nullptr, c.session_id, c.viewer_ip, "control");
    return;
  }
}

bool try_auth_client(ClientConn* c, const std::string& psk, session::SessionMutex* mutex,
                     MediaPlaneConfig::AuditFn audit_fn, void* audit_user,
                     std::vector<ClientConn>* clients) {
  using namespace road_desk::replace;
  if (!c || !c->tls || c->authed) {
    return true;
  }
  const SOCKET sock = tls::tls_get_socket(c->tls);
  const int pending = tls::tls_pending(c->tls);
  if (!socket_readable(sock, 0) && pending <= 0) {
    return true;
  }
  if (c->viewer_ip.empty()) {
    c->viewer_ip = socket_peer_ip(sock);
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
    emit_audit(audit_fn, audit_user, "failed", "auth_fail", "auth_fail", c->session_id,
               c->viewer_ip);
    return false;
  }
  std::string provided;
  std::string sid;
  if (!parse_auth_password(payload.data(), payload.size(), &provided, &sid)) {
    control_send_auth_fail(c->tls, "malformed auth");
    emit_audit(audit_fn, audit_user, "failed", "auth_fail", "auth_fail", c->session_id,
               c->viewer_ip);
    return false;
  }
  if (!sid.empty()) {
    c->session_id = std::move(sid);
  }
  if (!road_desk::session::authenticate_psk(psk, provided)) {
    control_send_auth_fail(c->tls, "auth failed");
    emit_audit(audit_fn, audit_user, "failed", "auth_fail", "auth_fail", c->session_id,
               c->viewer_ip);
    return false;
  }
  const int sw = GetSystemMetrics(SM_CXSCREEN);
  const int sh = GetSystemMetrics(SM_CYSCREEN);
  const uint16_t width = static_cast<uint16_t>(sw > 0 ? sw : 1);
  const uint16_t height = static_cast<uint16_t>(sh > 0 ? sh : 1);
  // A5a: first authed client gets control; others forced view-only.
  // Controller leave → ensure_one_controller promotes the earliest remaining viewer.
  const bool grant_control = !(clients && has_controller(*clients, c));
  const uint8_t role = grant_control ? kSessionRoleControl : kSessionRoleViewOnly;
  if (!control_send_auth_ok(c->tls, width, height, ROAD_DESK_VERSION_STRING, role)) {
    logf("send AuthOk failed");
    return false;
  }
  c->authed = true;
  c->can_control = grant_control;
  c->need_keyframe = true;
  c->clip.last_seq = GetClipboardSequenceNumber();
  c->cursor_hash_sent = 0;
  if (mutex) {
    mutex->try_acquire();
  }
  emit_audit(audit_fn, audit_user, "opened", "ok", nullptr, c->session_id, c->viewer_ip,
             grant_control ? "control" : "view_only");
  c->audit_opened = true;
  logf("client authed desktop=%ux%u role=%s viewers=%u", width, height,
       grant_control ? "control" : "view_only", mutex ? mutex->count() : 0);
  return true;
}

bool drain_all_clients(std::vector<ClientConn>* clients, const std::string& psk,
                       session::SessionMutex* mutex, MediaPlaneConfig::AuditFn audit_fn,
                       void* audit_user) {
  if (!clients) {
    return true;
  }
  for (ClientConn& c : *clients) {
    if (c.dead || !c.tls) {
      continue;
    }
    if (!c.authed) {
      if (!try_auth_client(&c, psk, mutex, audit_fn, audit_user, clients)) {
        close_client(&c, mutex, audit_fn, audit_user);
      }
      continue;
    }
    if (!drain_incoming(c.tls, &c.clip, c.can_control)) {
      logf("client gone (recv)");
      close_client(&c, mutex, audit_fn, audit_user);
    }
  }
  clients->erase(std::remove_if(clients->begin(), clients->end(),
                                [](const ClientConn& c) { return c.dead || !c.tls; }),
                 clients->end());
  ensure_one_controller(clients, audit_fn, audit_user);
  return true;
}

bool push_clipboard_one(ClientConn* c, uint32_t* shared_xfer_id,
                        std::vector<ClientConn>* all_for_pump, const std::string& psk,
                        session::SessionMutex* mutex, MediaPlaneConfig::AuditFn audit_fn,
                        void* audit_user) {
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
    return drain_all_clients(all_for_pump, psk, mutex, audit_fn, audit_user);
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
                           const std::string& psk, session::SessionMutex* mutex,
                           MediaPlaneConfig::AuditFn audit_fn, void* audit_user) {
  if (!clients) {
    return true;
  }
  for (ClientConn& c : *clients) {
    if (!c.authed || c.dead) {
      continue;
    }
    if (!push_clipboard_one(&c, shared_xfer_id, clients, psk, mutex, audit_fn, audit_user)) {
      close_client(&c, mutex, audit_fn, audit_user);
    }
  }
  clients->erase(std::remove_if(clients->begin(), clients->end(),
                                [](const ClientConn& c) { return c.dead || !c.tls; }),
                 clients->end());
  ensure_one_controller(clients, audit_fn, audit_user);
  return true;
}

void serve_shared(SOCKET listen_sock, const std::string& psk,
                  road_desk::replace::CaptureMode capture_mode,
                  road_desk::replace::CaptureStrategy capture_strategy,
                  const std::atomic<bool>* stop_requested, session::SessionMutex* mutex,
                  tls::HostCredentials* creds, MediaPlaneConfig::AuditFn audit_fn,
                  void* audit_user) {
  using namespace road_desk::replace;

  auto qpc_ms = []() -> double {
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) {
      QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
  };

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
    std::atomic<uint64_t> enc_iters_total{0};
    std::atomic<uint64_t> vout_iters_total{0};
    std::atomic<uint64_t> main_iters_total{0};
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
    // Video-out is latency-critical during drag: on a saturated VM (capture
    // BitBlt + WIC encode) default priority leaves it scheduled ~9x/s, so the
    // capture thread drops half its batches (outq full) and the dragged window
    // lags the mouse. Boost it so every captured batch reaches the wire.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    // --- temp diagnostics ---
    unsigned vout_msgs = 0;
    DWORD vout_lock_ms = 0;
    DWORD vout_write_ms = 0;
    unsigned vout_nw = 0;
    unsigned vout_backlog = 0;
    DWORD vout_last = GetTickCount();
    unsigned vout_empty = 0;
    double vout_qlock_ms = 0.0;
    double vout_qwrite_ms = 0.0;
    unsigned vout_iters = 0;
    // -------------------------
    VideoWriteYieldCtx yield_ctx{};
    yield_ctx.shared = &shared;
    while (!shared.video_out_stop.load() &&
           !(stop_requested && stop_requested->load())) {
      std::vector<uint8_t> msg;
      shared.vout_iters_total.fetch_add(1);
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
        ++vout_empty;
        Sleep(0);  // yield time-slice, don't park — drag respawns immediately
        continue;
      }

      shared.video_writing.store(true);
      // Prefer inject while dragging — but never spin-wait on g_inject_active: the
      // inject loop keeps that flag true almost continuously during a drag, so a
      // spin here starves video-out and collapses the drag frame rate (Win7 stutter,
      // Win10 cursor/window desync). Just take io_mu; the per-chunk yield below already
      // hands IO back to inject between TLS records, keeping input latency low.
      ++vout_iters;
      const double vout_q0 = qpc_ms();
      const DWORD vout_t0 = GetTickCount();
      std::unique_lock<std::mutex> lock(shared.io_mu);
      vout_lock_ms += GetTickCount() - vout_t0;
      vout_qlock_ms += qpc_ms() - vout_q0;
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
          ++vout_nw;
          // During drag, forcing keyframes just floods Win7 with full-desk JPEG.
          if ((g_ptr_buttons.load() & 1) == 0) {
            c.need_keyframe = true;
            g_any_need_keyframe.store(true);
          }
          continue;
        }
        all_blocked = false;
        const bool big = msg.size() >= 16u * 1024u;
        const DWORD vout_w0 = GetTickCount();
        const double vout_q1 = qpc_ms();
        const bool ok =
            big ? mux_write_yield(c.tls, kChannelVideo, msg.data(),
                                  static_cast<uint32_t>(msg.size()), video_write_yield,
                                  &yield_ctx)
                : mux_write(c.tls, kChannelVideo, msg.data(),
                            static_cast<uint32_t>(msg.size()));
        vout_write_ms += GetTickCount() - vout_w0;
        vout_qwrite_ms += qpc_ms() - vout_q1;
        if (!ok) {
          close_client(&c, mutex, audit_fn, audit_user);
          continue;
        }
        any_sent = true;
      }
      yield_ctx.lock = nullptr;
      lock.unlock();
      shared.video_writing.store(false);
      ++vout_msgs;
      if (vout_backlog == 0) {
        vout_backlog = static_cast<unsigned>(msg.size());
      }
      const DWORD vout_now = GetTickCount();
      if (vout_now - vout_last >= 1000) {
        logf("vout diag: msgs=%u empty=%u iters=%u qlock=%.1f qwrite=%.1f lock_ms=%u write_ms=%u nw=%u last_bytes=%u",
             vout_msgs, vout_empty, vout_iters, vout_qlock_ms, vout_qwrite_ms,
             static_cast<unsigned>(vout_lock_ms),
             static_cast<unsigned>(vout_write_ms), vout_nw, vout_backlog);
        vout_msgs = 0;
        vout_empty = 0;
        vout_iters = 0;
        vout_qlock_ms = 0.0;
        vout_qwrite_ms = 0.0;
        vout_lock_ms = 0;
        vout_write_ms = 0;
        vout_nw = 0;
        vout_last = vout_now;
      }
      vout_backlog = 0;
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
    double qpc_cap_ms = 0.0;
    double qpc_loop_ms = 0.0;
    unsigned enc_iters = 0;
    unsigned enc_empty = 0;
    // GDI drag region capture: anchor the dragged window once on LMB-down, then
    // BitBlt only the moving window + wake each tick (a full-screen GDI blit is
    // ~47ms on VMware SVGA and starves video-out / collapses the drag frame rate).
    bool gdi_drag_anchor_valid = false;
    bool gdi_drag_full_pending = true;
    bool force_kf_pending = true;
    bool was_drag_mode = false;
    bool cap_was_dxgi = true;
    int gdi_drag_win_x = 0;
    int gdi_drag_win_y = 0;
    int gdi_drag_win_w = 0;
    int gdi_drag_win_h = 0;
    int gdi_drag_prev_win_x = 0;
    int gdi_drag_prev_win_y = 0;
    int gdi_drag_anchor_mx = 0;
    int gdi_drag_anchor_my = 0;
    int gdi_drag_full_countdown = 0;
    // Live HWND of the dragged window (set on anchor; re-queried every region tick so
    // the capture region always covers the window at its true position).
    HWND gdi_drag_hwnd = nullptr;
    int desk_w = 0;
    int desk_h = 0;
    DWORD last_send_ms = 0;
    DWORD last_stat_ms = GetTickCount();
    DWORD last_attach_scrub_ms = GetTickCount();
    int idle_empties = 0;
    constexpr DWORD kStatIntervalMs = 1000;
    constexpr DWORD kAttachScrubMs = 2000;

    while (!shared.encode_stop.load() &&
           !(stop_requested && stop_requested->load())) {
      if (!shared.session_active.load()) {
        if (cap_begun) {
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
          gdi_drag_anchor_valid = false;
          gdi_drag_full_pending = true;
          force_kf_pending = true;
          was_drag_mode = false;
          gdi_drag_full_countdown = 0;
          gdi_drag_prev_win_x = 0;
          gdi_drag_prev_win_y = 0;
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
      shared.enc_iters_total.fetch_add(1);
      const double qpc_iter0 = qpc_ms();
      if (cap->using_mirror() && loop_now - last_attach_scrub_ms >= kAttachScrubMs) {
        last_attach_scrub_ms = loop_now;
        rdm_scrub_foreign_attach_registry();
      }

      const DWORD now = GetTickCount();
      const bool drag_mode = (g_ptr_buttons.load() & 1) != 0;
      // Non-drag: keep the 8ms floor so Explorer-select dirties don't spin too fast.
      // Drag: no floor — let capture + encode run at natural speed (VNC-style push).
      if (last_send_ms != 0 && now - last_send_ms < 8u && !drag_mode) {
        Sleep(1);
        continue;
      }

      bool outq_full = false;
      {
        std::lock_guard<std::mutex> lock(shared.outq_mu);
        const size_t max_b = kMaxBatches;  // keep pipeline depth during drag too
        if (shared.outq.size() >= max_b) {
          shared.dropped_ticks.fetch_add(1);
          shared.dropped_interval.fetch_add(1);
          outq_full = true;
        }
      }

      const bool any_kf = g_any_need_keyframe.load();
      int validate = g_validate_frames.load();
      // DXGI drag: never force_full_pixels — that clears MoveRects (lab copy=0).
      const bool force_validate =
          cap->provides_dirties() && validate > 0 && !(cap->using_dxgi() && drag_mode);
      // force_kf_pending is produced locally (session start / DXGI->GDI fallback) so a real
      // full-desk keyframe ships even while dragging; the old any_kf gate only worked outside drag.
      const bool force_full = !have_prev || force_kf_pending || (any_kf && !drag_mode);

      int w = 0;
      int h = 0;
      const double qpc_cap0 = qpc_ms();
      const DWORD cap0 = GetTickCount();
      mirror_dirties.clear();
      mirror_moves.clear();

      // --- GDI drag region capture ---
      bool gdi_region = false;
      int greg_x = 0;
      int greg_y = 0;
      int greg_w = 0;
      int greg_h = 0;
      // Window position sampled this tick; committed to gdi_drag_prev_win_* only
      // when the batch actually ships (the diff is against the last SENT frame).
      int gdi_tick_win_x = 0;
      int gdi_tick_win_y = 0;
      bool gdi_tick_win_valid = false;
      const bool gdi_backend = !cap->provides_dirties();
      const bool use_region = gdi_backend && drag_mode && !force_validate && !force_full;
      if (use_region) {
        if (!was_drag_mode || gdi_drag_full_pending || !gdi_drag_anchor_valid) {
          // Full capture this tick; the window anchor comes from WindowFromPoint below.
          gdi_drag_full_pending = false;
          if (!cap->capture(&frame, &w, &h, &mirror_dirties, &mirror_moves, false)) {
            Sleep(5);
            continue;
          }
          desk_w = w;
          desk_h = h;
        } else {
          const int mx = g_ptr_x.load();
          const int my = g_ptr_y.load();
          // Pointer motion since the last region tick: the window keeps moving
          // while the ~30-50ms BitBlt is in flight, so the region must extend
          // ahead of the motion or the leading edge lands outside the region.
          const int pdx = mx - gdi_drag_anchor_mx;
          const int pdy = my - gdi_drag_anchor_my;
          int wx = gdi_drag_win_x + (mx - gdi_drag_anchor_mx);
          int wy = gdi_drag_win_y + (my - gdi_drag_anchor_my);
          // Prefer the live window rect: the pointer-delta estimate runs ahead of the
          // real window when injected input lags, so the union region must track the
          // true old/new positions to wipe the exposed background behind the window.
          RECT live{};
          if (gdi_drag_hwnd && IsWindow(gdi_drag_hwnd) && GetWindowRect(gdi_drag_hwnd, &live) &&
              live.right > live.left && live.bottom > live.top) {
            wx = live.left;
            wy = live.top;
            gdi_drag_win_w = live.right - live.left;
            gdi_drag_win_h = live.bottom - live.top;
            gdi_drag_anchor_mx = mx;
            gdi_drag_anchor_my = my;
          }
          gdi_tick_win_x = wx;
          gdi_tick_win_y = wy;
          gdi_tick_win_valid = true;
          // Union of the window at the last SENT position and the current rect,
          // plus a motion lookahead: the background exposed at the old position
          // must be captured, otherwise ghost trails of the dragged window stay
          // on the viewer (capture_region only writes pixels inside the region).
          constexpr int kRegionMargin = 64;
          const int look_x = pdx > 0 ? 2 * pdx : -2 * pdx;
          const int look_y = pdy > 0 ? 2 * pdy : -2 * pdy;
          int rx0 = (wx < gdi_drag_prev_win_x ? wx : gdi_drag_prev_win_x) - kRegionMargin;
          int ry0 = (wy < gdi_drag_prev_win_y ? wy : gdi_drag_prev_win_y) - kRegionMargin;
          int rx1 = (wx + gdi_drag_win_w > gdi_drag_prev_win_x + gdi_drag_win_w
                         ? wx + gdi_drag_win_w
                         : gdi_drag_prev_win_x + gdi_drag_win_w) +
                    kRegionMargin;
          int ry1 = (wy + gdi_drag_win_h > gdi_drag_prev_win_y + gdi_drag_win_h
                         ? wy + gdi_drag_win_h
                         : gdi_drag_prev_win_y + gdi_drag_win_h) +
                    kRegionMargin;
          if (pdx < 0) {
            rx0 -= look_x;
          } else {
            rx1 += look_x;
          }
          if (pdy < 0) {
            ry0 -= look_y;
          } else {
            ry1 += look_y;
          }
          greg_x = rx0;
          greg_y = ry0;
          greg_w = rx1 - rx0;
          greg_h = ry1 - ry0;
          if (greg_x < 0) {
            greg_w += greg_x;
            greg_x = 0;
          }
          if (greg_y < 0) {
            greg_h += greg_y;
            greg_y = 0;
          }
          if (greg_x + greg_w > desk_w) {
            greg_w = desk_w - greg_x;
          }
          if (greg_y + greg_h > desk_h) {
            greg_h = desk_h - greg_y;
          }
          if (greg_w > 0 && greg_h > 0 &&
              cap->capture_gdi_region(&frame, greg_x, greg_y, greg_w, greg_h)) {
            gdi_region = true;
          } else {
            if (!cap->capture(&frame, &w, &h, &mirror_dirties, &mirror_moves, false)) {
              Sleep(5);
              continue;
            }
            desk_w = w;
            desk_h = h;
          }
        }
      } else {
        if (!cap->capture(&frame, &w, &h, &mirror_dirties, &mirror_moves, force_validate)) {
          Sleep(5);
          continue;
        }
        desk_w = w;
        desk_h = h;
      }
      if (gdi_region) {
        w = desk_w;
        h = desk_h;
      }
      const DWORD cap_ms = GetTickCount() - cap0;
      qpc_cap_ms += qpc_ms() - qpc_cap0;

      // DXGI -> GDI fallback (VMware DDA black-frame quirk): the client may still
      // hold the black keyframe. Force a real full-desk keyframe so the viewer's
      // desktop matches the host instead of showing only incremental rects.
      if (cap_was_dxgi && !cap->using_dxgi()) {
        cap_was_dxgi = false;
        g_any_need_keyframe.store(true);
        force_kf_pending = true;
        have_prev = false;
        prev.clear();
        gdi_drag_anchor_valid = false;
        gdi_drag_full_pending = true;
        was_drag_mode = false;
        gdi_drag_full_countdown = 0;
        logf("capture backend dxgi->gdi: forcing full keyframe");
      } else if (!cap_was_dxgi && cap->using_dxgi()) {
        cap_was_dxgi = true;
      }

      // Always pump capture (esp. DXGI) even when outq is full — skipping AcquireNextFrame
      // coalesces MoveRects into a full DirtyRect (lab: copy=0 + 1MB zlib).
      if (outq_full) {
        // Pump capture while the out queue is full, but do NOT advance prev: the
        // next sent batch must diff against the last SENT frame. Advancing prev
        // here meant window-move regions between sent frames were never covered,
        // leaving ghost trails on the viewer during drag.
        // During drag, drop the tick immediately — outq being full means video-out
        // is slow (TCP backpressure). The encode thread busy-waiting the capture
        // loop only produces frames that will be evicted, wasting CPU and starving
        // the video-out thread that shares the same core(s). Skipping the tick
        // without sleeping lets the next loop iteration check outq again at
        // natural speed.
        continue;
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

      // Non-drag: OS dirties (DXGI/Mirror) are often coarse (whole Explorer list pane).
      // Refine with pixel block-diff so selection highlights don't ship as large JPEG.
      if (cap->provides_dirties() && !force_validate && !force_full && !drag_mode) {
        if (have_prev && !prev.empty() && prev.size() == frame.size()) {
          std::vector<DirtyRect> refined;
          for (const CaptureDirty& d : mirror_dirties) {
            int x = d.x;
            int y = d.y;
            int rw = d.w;
            int rh = d.h;
            if (x < 0) {
              rw += x;
              x = 0;
            }
            if (y < 0) {
              rh += y;
              y = 0;
            }
            if (x + rw > w) {
              rw = w - x;
            }
            if (y + rh > h) {
              rh = h - y;
            }
            if (rw <= 0 || rh <= 0) {
              continue;
            }
            std::vector<DirtyRect> part;
            collect_dirty_rects_region(frame.data(), prev.data(), w, x, y, rw, rh, &part);
            refined.insert(refined.end(), part.begin(), part.end());
          }
          coalesce_vertical(&refined);
          dirties = std::move(refined);
        } else {
          for (const CaptureDirty& d : mirror_dirties) {
            dirties.push_back(DirtyRect{d.x, d.y, d.w, d.h});
          }
        }
      } else if (gdi_region) {
        collect_dirty_rects_region(frame.data(), prev.data(), w, greg_x, greg_y, greg_w, greg_h,
                                   &dirties);
        // Escape detection: changes near the region border mean the window (or
        // another window) may have moved outside it — force a full capture next
        // tick so nothing goes stale.
        constexpr int kEscapeMargin = 24;
        for (const DirtyRect& r : dirties) {
          if (r.x - greg_x < kEscapeMargin || greg_x + greg_w - (r.x + r.w) < kEscapeMargin ||
              r.y - greg_y < kEscapeMargin || greg_y + greg_h - (r.y + r.h) < kEscapeMargin) {
            gdi_drag_full_pending = true;
            break;
          }
        }
      } else {
        collect_dirty_rects(frame.data(), have_prev && !force_full ? prev.data() : nullptr, w, h,
                            force_full, &dirties);
      }

      // GDI drag: on full ticks, anchor the dragged window from the *real* window
      // under the pointer (WindowFromPoint + GetWindowRect). The dirty AABB on the
      // LMB-down tick can cover only part of the window (e.g. just the title bar),
      // which leaves the rest outside the capture region and shows ghost trails.
      // Fall back to the AABB only when the window lookup is not plausible.
      if (use_region && !gdi_region) {
        const int mx = g_ptr_x.load();
        const int my = g_ptr_y.load();
        bool anchored = false;
        POINT pt{};
        pt.x = mx;
        pt.y = my;
        if (HWND hwnd = WindowFromPoint(pt)) {
          HWND root = GetAncestor(hwnd, GA_ROOT);
          RECT wr{};
          if (root && IsWindow(root) && GetWindowRect(root, &wr)) {
            const int rw = wr.right - wr.left;
            const int rh = wr.bottom - wr.top;
            // Sanity: a plausible draggable window that contains the pointer and is
            // not (nearly) the whole desktop.
            if (rw > 0 && rh > 0 && mx >= wr.left && mx < wr.right && my >= wr.top &&
                my < wr.bottom &&
                static_cast<int64_t>(rw) * rh * 2 < static_cast<int64_t>(desk_w) * desk_h) {
              gdi_drag_win_x = wr.left;
              gdi_drag_win_y = wr.top;
              gdi_drag_win_w = rw;
              gdi_drag_win_h = rh;
              gdi_drag_anchor_mx = mx;
              gdi_drag_anchor_my = my;
              gdi_drag_anchor_valid = true;
              gdi_tick_win_x = wr.left;
              gdi_tick_win_y = wr.top;
              gdi_tick_win_valid = true;
              gdi_drag_hwnd = root;
              gdi_drag_full_countdown = 8;
              anchored = true;
              logf("gdi drag anchor: window %dx%d@%d,%d ptr=%d,%d", rw, rh, wr.left, wr.top,
                   mx, my);
            }
          }
        }
        if (!anchored) {
          int x0 = w;
          int y0 = h;
          int x1 = 0;
          int y1 = 0;
          for (const DirtyRect& r : dirties) {
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
          // Sanity: only anchor when the diff is a small window, not a full-screen
          // sweep (e.g. restore-from-maximized animation).
          const bool sane =
              x1 > x0 && y1 > y0 &&
              static_cast<int64_t>(x1 - x0) * (y1 - y0) * 2 < static_cast<int64_t>(w) * h;
          if (sane) {
            gdi_drag_win_x = x0;
            gdi_drag_win_y = y0;
            gdi_drag_win_w = x1 - x0;
            gdi_drag_win_h = y1 - y0;
            gdi_drag_anchor_mx = mx;
            gdi_drag_anchor_my = my;
            gdi_drag_anchor_valid = true;
            gdi_tick_win_x = x0;
            gdi_tick_win_y = y0;
            gdi_tick_win_valid = true;
            gdi_drag_hwnd = nullptr;
            gdi_drag_full_countdown = 8;
            anchored = true;
            logf("gdi drag anchor fallback(aabb): %dx%d@%d,%d", x1 - x0, y1 - y0, x0, y0);
          }
        }
      } else if (use_region && gdi_region && --gdi_drag_full_countdown <= 0) {
        gdi_drag_full_pending = true;
      }
      was_drag_mode = drag_mode;

      // Win7/Mirror: no pixel CopyRect (flooded IO or capped to useless). Dirties only.

      if (force_validate && validate > 0) {
        g_validate_frames.store(validate - 1);
      }

      if (dirties.empty() && copies.empty()) {
        ++enc_empty;
        // Idle backoff: an unchanged desktop used to spin the capture loop at
        // thousands of Hz (win7 Mirror full-DIB memcpy) or ~30 Hz (win10 GDI full
        // BitBlt), pegging a core with nothing to send. Sleep grows to 16ms and
        // resets on the next shipped tick; worst-case 16ms poll keeps drag-start
        // latency negligible.
        constexpr int kIdleSleepMs[] = {0, 2, 4, 8, 16, 16};
        const int lvl = idle_empties < 6 ? idle_empties : 5;
        Sleep(kIdleSleepMs[lvl]);
        ++idle_empties;
        continue;
      }

      const unsigned before_collapse = static_cast<unsigned>(dirties.size() + copies.size());
      // Keep dirty strips (moving window = thin edges + exposed background) instead
      // of one giant AABB; collapse only on fragment storms / mostly-dirty. DXGI
      // with CopyRects must not union dirties into a near-full zlib either.
      collapse_dirties_for_send(&dirties, w, h, drag_mode && copies.empty());

      const bool full_keyframe =
          dirties.size() == 1 && dirties[0].x == 0 && dirties[0].y == 0 && dirties[0].w == w &&
          dirties[0].h == h;
      // Split large/non-drag updates into strips (incl. accidental full KF) so we never
      // ship one Viewer-wide JPEG from Explorer UI updates.
      if (!full_keyframe || !drag_mode) {
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
      // Rects that actually encoded OK; only these may advance prev (a failed
      // rect must stay diffed so the next tick re-sends it - otherwise the
      // viewer misses those pixels forever and shows ghost trails).
      std::vector<DirtyRect> sent_dirties;
      sent_dirties.reserve(dirties.size());

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
        // After first KF: never ship full-desk zlib (~1MB) on drag — holds io_mu.
        // Non-drag: never JPEG/H264 (Explorer select dirties whole window via DWM;
        // lossy recompress flashes the entire Viewer even when Host barely changes).
        const bool huge = area * 2u > desk_area;
        const bool allow_lossy = drag_mode;
        // Drag: JPEG only — H264 encode latency makes Viewer cursor lead the window.
        const bool try_h264 = false;  // non-drag disabled; drag uses JPEG below
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
        // Drag+huge: skip megabyte zlib. Non-drag strips should be small enough for zlib.
        if (!ok && !(have_prev && huge && drag_mode)) {
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
        sent_dirties.push_back(r);
        ++sent_rects;
        ++frame_id;
      }

      if (!batch_ok) {
        shared.dropped_ticks.fetch_add(1);
        shared.dropped_interval.fetch_add(1);
        continue;
      }

      bool shipped = false;
      bool evicted = false;
      unsigned evict_this = 0;
      {
        std::lock_guard<std::mutex> lock(shared.outq_mu);
        const size_t max_b = (g_ptr_buttons.load() & 1) ? 1u : kMaxBatches;
        while (shared.outq.size() >= max_b) {
          shared.outq.pop_front();
          // Evicting a queued-but-unsent batch desyncs prev from the viewer: the
          // rects were applied to prev but never reached the client, so the next
          // diff would skip them. Skip prev advancement when eviction happens and
          // let the following tick re-diff those areas (self-healing, no ghosts).
          evicted = true;
          ++evict_this;
          shared.dropped_ticks.fetch_add(1);
          shared.dropped_interval.fetch_add(1);
        }
        if (!tick_batch.msgs.empty()) {
          shared.outq.push_back(std::move(tick_batch));
          shipped = true;
        }
      }
      if (shipped && !evicted && use_region && gdi_tick_win_valid) {
        // The shipped diff is against the last SENT frame, so the region anchor
        // must track the last sent window position; dropped ticks (outq full)
        // must not advance it or the next union misses the window's old spot.
        gdi_drag_prev_win_x = gdi_tick_win_x;
        gdi_drag_prev_win_y = gdi_tick_win_y;
      }

      g_video_ptr_x.store(g_ptr_x.load());
      g_video_ptr_y.store(g_ptr_y.load());
      g_video_ptr_have.store(true);
      if (!evicted) {
        force_kf_pending = false;
        if (full_keyframe) {
          g_any_need_keyframe.store(false);
          // Per-client flags were never cleared (lab Win7: force_full every tick → 220KB JPEG storm).
          g_clear_client_keyframes.store(true);
        }
      }

      if (shipped && !evicted) {
        for (const CopyRect& cr : copies) {
          copy_rect_to_prev(prev.data(), frame.data(), w, h, DirtyRect{cr.dx, cr.dy, cr.w, cr.h});
        }
        for (const DirtyRect& r : sent_dirties) {
          copy_rect_to_prev(prev.data(), frame.data(), w, h, r);
        }
        have_prev = true;
      }
      last_send_ms = GetTickCount();
      ++ticks_sent;
      idle_empties = 0;

      ++enc_iters;
      qpc_loop_ms += qpc_ms() - qpc_iter0;
      if (ticks_sent == 1 || last_send_ms - last_stat_ms >= kStatIntervalMs) {
        last_stat_ms = last_send_ms;
        size_t outq_n = 0;
        {
          std::lock_guard<std::mutex> lock(shared.outq_mu);
          outq_n = shared.outq.size();
        }
        const uint32_t di = shared.dropped_interval.exchange(0);
        const ProcStats ps = sample_proc_stats();
        logf("tick=%u batch=%u(from %u) kf=%u copy=%u moves_in=%u dirty_in=%u wire=%u zlib=%u jpeg=%u "
             "h264=%u cap_ms=%u qcap=%.1f qloop=%.1f iters=%u empty=%u enc_ms=%u raw=%u rect=%ux%u@%u,%u greg=%s outq=%u evict=%u drop=%u drop_total=%u cpu1=%.0f cpuN=%.0f mem=%llu mem%%=%.1f",
             ticks_sent, static_cast<unsigned>(dirties.size() + copies.size()), before_collapse,
             full_keyframe ? 1u : 0u, copy_this, moves_in, dirties_in,
             static_cast<unsigned>(wire_this), zlib_this,
             jpeg_this, h264_this, cap_ms, qpc_cap_ms, qpc_loop_ms, enc_iters, enc_empty,
             enc_this, static_cast<unsigned>(raw_this),
             dirties.empty() ? 0u : static_cast<unsigned>(dirties[0].w),
             dirties.empty() ? 0u : static_cast<unsigned>(dirties[0].h),
             dirties.empty() ? 0 : dirties[0].x, dirties.empty() ? 0 : dirties[0].y,
             (gdi_region ? (std::to_string(greg_w) + "x" + std::to_string(greg_h) + "@" +
                            std::to_string(greg_x) + "," + std::to_string(greg_y))
                         : std::string("full"))
                 .c_str(),
             static_cast<unsigned>(outq_n), evict_this, di,
             shared.dropped_ticks.load(), ps.cpu_one_core_pct, ps.cpu_machine_pct,
             static_cast<unsigned long long>(ps.ws_bytes >> 20), ps.mem_pct);
        qpc_cap_ms = 0.0;
        qpc_loop_ms = 0.0;
        enc_iters = 0;
        enc_empty = 0;
      }
    }

    if (cap_begun) {
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

  std::thread watchdog_thr([&] {
    uint64_t last_e = 0;
    uint64_t last_v = 0;
    uint64_t last_m = 0;
    while (!(stop_requested && stop_requested->load())) {
      Sleep(2000);
      const uint64_t e = shared.enc_iters_total.load();
      const uint64_t v = shared.vout_iters_total.load();
      const uint64_t m = shared.main_iters_total.load();
      const ProcStats ps_wd = sample_proc_stats();
      logf("wd: enc=%llu(+%llu) vout=%llu(+%llu) main=%llu(+%llu) outq=%llu drop=%u cpu1=%.0f cpuN=%.0f mem=%llu mem%%=%.1f",
           static_cast<unsigned long long>(e),
           static_cast<unsigned long long>(e - last_e),
           static_cast<unsigned long long>(v),
           static_cast<unsigned long long>(v - last_v),
           static_cast<unsigned long long>(m),
           static_cast<unsigned long long>(m - last_m),
           static_cast<unsigned long long>(shared.outq.size()),
           shared.dropped_ticks.load(), ps_wd.cpu_one_core_pct, ps_wd.cpu_machine_pct,
           static_cast<unsigned long long>(ps_wd.ws_bytes >> 20), ps_wd.mem_pct);
      last_e = e;
      last_v = v;
      last_m = m;
    }
  });

  DWORD last_cursor_ms = 0;
  DWORD last_alive_ms = GetTickCount();
  bool logged_io_alive = false;

  while (!(stop_requested && stop_requested->load()) && listen_sock != INVALID_SOCKET) {
    shared.main_iters_total.fetch_add(1);
    const bool dragging = (g_ptr_buttons.load() & 1) != 0;

    g_inject_active.store(true);
    {
      std::lock_guard<std::mutex> lock(shared.io_mu);
      drain_all_clients(&shared.clients, psk, mutex, audit_fn, audit_user);
      if (!dragging && !shared.video_writing.load()) {
        fanout_host_clipboard(&shared.clients, &shared_xfer_id, psk, mutex, audit_fn, audit_user);
      }
    }
    flush_pending_input();
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
    // Dragging: 1ms poll keeps input latency low without letting the inject loop
    // busy-spin (select 0) monopolize a weak VM core and starve video-out.
    tv.tv_usec = (authed == 0) ? 50000 : (dragging ? 1000 : 2000);
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
        const std::string peer = socket_peer_ip(tcp);
        std::lock_guard<std::mutex> lock(shared.io_mu);
        if (shared.clients.size() >= kMaxClients) {
          logf("reject: max clients (%u) peer=%s", static_cast<unsigned>(kMaxClients),
               peer.c_str());
          emit_audit(audit_fn, audit_user, "failed", "capacity_reject", "capacity_reject",
                     std::string(), peer);
          closesocket(tcp);
        } else if (!creds) {
          closesocket(tcp);
        } else {
          tls::TlsSession* tls_sess = tls::server_handshake(tcp, *creds);
          if (!tls_sess) {
            logf("TLS handshake failed peer=%s", peer.c_str());
            emit_audit(audit_fn, audit_user, "failed", "tls_fail", "tls_fail", std::string(),
                       peer);
          } else {
            ClientConn c;
            c.tls = tls_sess;
            c.viewer_ip = peer;
            shared.clients.push_back(std::move(c));
            logf("TLS up clients=%u/%u peer=%s - awaiting auth",
                 static_cast<unsigned>(shared.clients.size()),
                 static_cast<unsigned>(kMaxClients), peer.c_str());
          }
        }
      }
    }

    g_inject_active.store(true);
    {
      std::lock_guard<std::mutex> lock(shared.io_mu);
      drain_all_clients(&shared.clients, psk, mutex, audit_fn, audit_user);
    }
    flush_pending_input();
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
          close_client(&c, mutex, audit_fn, audit_user);
        }
      }
      shared.clients.erase(std::remove_if(shared.clients.begin(), shared.clients.end(),
                                          [](const ClientConn& c) { return c.dead || !c.tls; }),
                           shared.clients.end());
    }

    g_inject_active.store(true);
    {
      std::lock_guard<std::mutex> lock(shared.io_mu);
      drain_all_clients(&shared.clients, psk, mutex, audit_fn, audit_user);
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
    flush_pending_input();
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
  if (watchdog_thr.joinable()) {
    watchdog_thr.join();
  }

  {
    std::lock_guard<std::mutex> lock(shared.io_mu);
    for (ClientConn& c : shared.clients) {
      close_client(&c, mutex, audit_fn, audit_user);
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
  MediaPlaneConfig::AuditFn audit_fn = nullptr;
  void* audit_user = nullptr;
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
  impl_->audit_fn = config.audit_fn;
  impl_->audit_user = config.audit_user;
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
               &impl_->stop_requested, impl_->session_mutex, &impl_->tls_creds, impl_->audit_fn,
               impl_->audit_user);

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
