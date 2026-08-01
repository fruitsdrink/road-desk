// Track P + M2: TLS mux Video/Input; capture = Mirror dirties (default) or GDI.
// Single-thread session I/O (select + send) to avoid TLS read/write deadlock.

#include "cursor_capture.h"
#include "inject.h"
#include "log_util.h"
#include "mirror_capture.h"
#include "mux.h"
#include "protocol.h"

#include "auth.h"
#include "session_mutex.h"
#include "tls_schannel.h"

#include "miniz.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void logf(const char* fmt, ...) {
  char line[2048];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  road_desk::replace::logf("replace-host", "%s", line);
}

bool wsa_init() {
  WSADATA wsa{};
  return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
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

struct SendRectStats {
  uint32_t raw_bytes = 0;
  uint32_t wire_bytes = 0;
  uint8_t codec = 0;
  DWORD encode_ms = 0;
  DWORD send_ms = 0;
};

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

bool send_video_rect(road_desk::media::tls::TlsSession* tls, uint32_t frame_id, int desk_w,
                     int desk_h, int x, int y, int rw, int rh, const uint8_t* bgra_full,
                     std::vector<uint8_t>* raw_buf, std::vector<uint8_t>* body_buf,
                     SendRectStats* st) {
  using namespace road_desk::replace;
  if (!tls || !bgra_full || !raw_buf || !body_buf ||
      !rect_in_desk(desk_w, desk_h, x, y, rw, rh)) {
    return false;
  }
  const uint32_t pix = static_cast<uint32_t>(rw) * static_cast<uint32_t>(rh) * 4u;
  if (pix == 0 || static_cast<uint32_t>(kVideoHeaderSize) + pix > kMaxPayloadLen) {
    return false;
  }
  const DWORD t0 = GetTickCount();
  raw_buf->resize(pix);
  uint8_t* raw = raw_buf->data();
  for (int row = 0; row < rh; ++row) {
    const uint8_t* src = bgra_full + (static_cast<size_t>(y + row) * desk_w + x) * 4u;
    std::memcpy(raw + static_cast<size_t>(row) * rw * 4u, src, static_cast<size_t>(rw) * 4u);
  }

  // Prefer zlib (level 1) when it shrinks; else raw.
  mz_ulong bound = mz_compressBound(pix);
  body_buf->resize(static_cast<size_t>(kVideoHeaderSize) + static_cast<size_t>(bound));
  uint8_t* body = body_buf->data();
  write_u32_le(body + 1, frame_id);
  write_u16_le(body + 5, static_cast<uint16_t>(x));
  write_u16_le(body + 7, static_cast<uint16_t>(y));
  write_u16_le(body + 9, static_cast<uint16_t>(rw));
  write_u16_le(body + 11, static_cast<uint16_t>(rh));

  mz_ulong zlen = bound;
  const int zrc = mz_compress2(body + kVideoHeaderSize, &zlen, raw, pix, MZ_BEST_SPEED);
  uint32_t total = 0;
  uint8_t codec = kVideoRawBgra;
  if (zrc == MZ_OK && zlen > 0 && zlen < pix) {
    body[0] = kVideoZlibBgra;
    codec = kVideoZlibBgra;
    total = static_cast<uint32_t>(kVideoHeaderSize) + static_cast<uint32_t>(zlen);
  } else {
    body_buf->resize(static_cast<size_t>(kVideoHeaderSize) + pix);
    body = body_buf->data();
    body[0] = kVideoRawBgra;
    write_u32_le(body + 1, frame_id);
    write_u16_le(body + 5, static_cast<uint16_t>(x));
    write_u16_le(body + 7, static_cast<uint16_t>(y));
    write_u16_le(body + 9, static_cast<uint16_t>(rw));
    write_u16_le(body + 11, static_cast<uint16_t>(rh));
    std::memcpy(body + kVideoHeaderSize, raw, pix);
    codec = kVideoRawBgra;
    total = static_cast<uint32_t>(kVideoHeaderSize) + pix;
  }
  const DWORD t1 = GetTickCount();
  // Note: caller drains Input between rects; mux_write itself is still blocking.
  const bool ok = mux_write(tls, kChannelVideo, body, total);
  const DWORD t2 = GetTickCount();
  if (st) {
    st->raw_bytes = pix;
    st->wire_bytes = total;
    st->codec = codec;
    st->encode_ms = t1 - t0;
    st->send_ms = t2 - t1;
  }
  return ok;
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

// Merge same-width bands stacked vertically (cuts mux storms during drag).
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

// Block dirty detect + horizontal coalesce (aligned with LibVNC kBlock=32).
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

// Only collapse when fragment storm or mostly-dirty. Do NOT bbox normal drag
// (old+new window bbox includes the gap and tanks FPS).
void collapse_dirties_for_send(std::vector<DirtyRect>* rects, int desk_w, int desk_h) {
  if (!rects || rects->size() <= 1 || desk_w <= 0 || desk_h <= 0) {
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

bool rect_pixels_match(const uint8_t* cur, const uint8_t* prev, int desk_w, int dx, int dy,
                       int sx, int sy, int w, int h) {
  // cur[dx,dy,w,h] == prev[sx,sy,w,h]
  for (int row = 0; row < h; ++row) {
    const uint8_t* a = cur + (static_cast<size_t>(dy + row) * desk_w + dx) * 4u;
    const uint8_t* b = prev + (static_cast<size_t>(sy + row) * desk_w + sx) * 4u;
    if (std::memcmp(a, b, static_cast<size_t>(w) * 4u) != 0) {
      return false;
    }
  }
  return true;
}

// When pointer moved by (mdx,mdy) during drag, try to turn dirty tiles into CopyRect.
void try_extract_copyrects(const uint8_t* cur, const uint8_t* prev, int desk_w, int desk_h,
                           int mdx, int mdy, std::vector<DirtyRect>* dirties,
                           std::vector<CopyRect>* copies) {
  copies->clear();
  if (!cur || !prev || !dirties || dirties->empty()) {
    return;
  }
  if (mdx == 0 && mdy == 0) {
    return;
  }
  if (mdx > 600 || mdx < -600 || mdy > 600 || mdy < -600) {
    return;
  }
  std::vector<DirtyRect> remain;
  remain.reserve(dirties->size());
  for (const DirtyRect& r : *dirties) {
    const int sx = r.x - mdx;
    const int sy = r.y - mdy;
    if (!rect_in_desk(desk_w, desk_h, sx, sy, r.w, r.h) ||
        !rect_in_desk(desk_w, desk_h, r.x, r.y, r.w, r.h)) {
      remain.push_back(r);
      continue;
    }
    if (rect_pixels_match(cur, prev, desk_w, r.x, r.y, sx, sy, r.w, r.h)) {
      copies->push_back({sx, sy, r.x, r.y, r.w, r.h});
    } else {
      remain.push_back(r);
    }
  }
  *dirties = std::move(remain);
}

bool send_copy_rect(road_desk::media::tls::TlsSession* tls, uint32_t frame_id,
                    const CopyRect& r) {
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
int g_ptr_buttons = 0;
int g_ptr_x = 0;
int g_ptr_y = 0;
int g_ptr_x_prev = 0;
int g_ptr_y_prev = 0;
bool g_ptr_have_prev = false;
uint32_t g_cursor_hash_sent = 0;
uint32_t g_copyrects_sent = 0;

bool send_cursor_shape(road_desk::media::tls::TlsSession* tls,
                       const road_desk::replace::CursorShape& c) {
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

bool maybe_send_cursor(road_desk::media::tls::TlsSession* tls) {
  road_desk::replace::CursorShape shape;
  if (!road_desk::replace::capture_cursor_shape(&shape)) {
    return true;
  }
  if (shape.hash == g_cursor_hash_sent) {
    return true;
  }
  if (!send_cursor_shape(tls, shape)) {
    return false;
  }
  g_cursor_hash_sent = shape.hash;
  return true;
}

// Slice tall rects so Input can be drained between mux writes (reduces drag "float").
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
    g_ptr_buttons = payload[1];
    const int x = read_u16_le(payload.data() + 2);
    const int y = read_u16_le(payload.data() + 4);
    if (g_ptr_have_prev) {
      g_ptr_x_prev = g_ptr_x;
      g_ptr_y_prev = g_ptr_y;
    } else {
      g_ptr_x_prev = x;
      g_ptr_y_prev = y;
      g_ptr_have_prev = true;
    }
    g_ptr_x = x;
    g_ptr_y = y;
    inject_pointer(payload[1], static_cast<uint16_t>(x), static_cast<uint16_t>(y));
    return;
  }
  if (payload[0] == kInputKey && payload.size() >= kInputKeySize) {
    ++g_input_key;
    inject_vk(read_u16_le(payload.data() + 2), payload[1] != 0);
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

// Drain Input/Control while data is pending (Input before next Video send).
bool drain_incoming(road_desk::media::tls::TlsSession* tls) {
  using namespace road_desk::replace;
  const SOCKET sock = road_desk::media::tls::tls_get_socket(tls);
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
    }
  }
  return true;
}

void serve_one(road_desk::media::tls::TlsSession* tls, const std::string& psk,
               road_desk::replace::CaptureMode capture_mode) {
  using namespace road_desk::replace;
  uint8_t ch = 0;
  std::vector<uint8_t> payload;
  if (!mux_read(tls, &ch, &payload)) {
    logf("read failed before auth (disconnect)");
    return;
  }
  if (ch != kChannelControl || payload.empty()) {
    logf("expected Control auth, got channel=%u len=%zu", static_cast<unsigned>(ch),
         payload.size());
    control_send_auth_fail(tls, "expected control auth");
    return;
  }
  std::string provided;
  if (!parse_auth_password(payload.data(), payload.size(), &provided)) {
    logf("malformed auth");
    control_send_auth_fail(tls, "malformed auth");
    return;
  }
  if (!road_desk::session::authenticate_psk(psk, provided)) {
    logf("auth failed (psk mismatch)");
    control_send_auth_fail(tls, "auth failed");
    return;
  }

  const int sw = GetSystemMetrics(SM_CXSCREEN);
  const int sh = GetSystemMetrics(SM_CYSCREEN);
  const uint16_t width = static_cast<uint16_t>(sw > 0 ? sw : 1);
  const uint16_t height = static_cast<uint16_t>(sh > 0 ? sh : 1);
  if (!control_send_auth_ok(tls, width, height)) {
    logf("send AuthOk failed");
    return;
  }
  SessionCapture cap(capture_mode);
  if (!cap.begin()) {
    logf("capture begin failed (mirror required but attach failed - install rdm_xpdm)");
    return;
  }
  if (cap.using_mirror()) {
    logf("auth ok desktop=%ux%u capture=mirror device=%s - M2 cursor+CopyRect+zlib", width,
         height, cap.mirror_device());
  } else {
    logf("auth ok desktop=%ux%u capture=gdi - G3 cursor+CopyRect+zlib", width, height);
  }
  // Tick lines scrolling in the captured console create a permanent dirty loop.
  road_desk::replace::log_set_mirror_stderr(false);
  g_input_pointer = 0;
  g_input_key = 0;
  g_ptr_have_prev = false;
  g_cursor_hash_sent = 0;
  g_copyrects_sent = 0;

  std::vector<uint8_t> frame;
  std::vector<uint8_t> prev;
  std::vector<uint8_t> raw_buf;
  std::vector<uint8_t> send_buf;
  std::vector<DirtyRect> dirties;
  std::vector<CaptureDirty> mirror_dirties;
  std::vector<CopyRect> copies;
  uint32_t frame_id = 0;
  uint32_t sent_rects = 0;
  uint32_t dropped_ticks = 0;
  uint32_t ticks_sent = 0;
  uint64_t sum_raw = 0;
  uint64_t sum_wire = 0;
  uint64_t sum_encode_ms = 0;
  uint64_t sum_send_ms = 0;
  uint32_t zlib_rects = 0;
  uint32_t raw_rects = 0;
  bool have_prev = false;
  DWORD last_send_ms = 0;
  DWORD last_stat_ms = GetTickCount();
  DWORD last_cursor_ms = 0;
  constexpr DWORD kStatIntervalMs = 1000;

  for (;;) {
    if (!drain_incoming(tls)) {
      logf("client gone (recv) sent_rects=%u drop_ticks=%u input_ptr=%u input_key=%u copy=%u",
           sent_rects, dropped_ticks, g_input_pointer, g_input_key, g_copyrects_sent);
      break;
    }

    int w = 0;
    int h = 0;
    const DWORD cap0 = GetTickCount();
    mirror_dirties.clear();
    if (!cap.capture(&frame, &w, &h, &mirror_dirties)) {
      Sleep(20);
      continue;
    }
    const DWORD cap_ms = GetTickCount() - cap0;

    const SOCKET sock = road_desk::media::tls::tls_get_socket(tls);
    const DWORD now = GetTickCount();
    if (now - last_cursor_ms >= 50) {
      last_cursor_ms = now;
      if (!maybe_send_cursor(tls)) {
        logf("cursor send failed - client gone?");
        break;
      }
    }

    const DWORD min_interval_ms = (g_ptr_buttons & 1) ? 4u : 8u;
    const bool interval_ok = (last_send_ms == 0) || (now - last_send_ms >= min_interval_ms);
    if (!interval_ok) {
      socket_readable(sock, 2);
      continue;
    }

    if (have_prev && !socket_writable(sock, 0)) {
      ++dropped_ticks;
      if ((dropped_ticks % 30) == 1) {
        logf("backpressure drop_ticks=%u (socket not writable, keep prev)", dropped_ticks);
      }
      socket_readable(sock, 4);
      continue;
    }

    const size_t frame_bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    if (frame.size() != frame_bytes || frame.empty()) {
      logf("capture size mismatch frame=%zu expect=%zu - skip", frame.size(), frame_bytes);
      Sleep(20);
      continue;
    }
    if (have_prev && prev.size() != frame_bytes) {
      logf("desktop size changed prev=%zu now=%zu - keyframe", prev.size(), frame_bytes);
      have_prev = false;
    }
    if (!have_prev) {
      prev.assign(frame_bytes, 0);
    }

    if (cap.using_mirror()) {
      dirties.clear();
      if (!have_prev && mirror_dirties.empty()) {
        dirties.push_back(DirtyRect{0, 0, w, h});
      } else {
        for (const CaptureDirty& d : mirror_dirties) {
          dirties.push_back(DirtyRect{d.x, d.y, d.w, d.h});
        }
      }
    } else {
      collect_dirty_rects(frame.data(), have_prev ? prev.data() : nullptr, w, h, !have_prev,
                          &dirties);
    }
    if (dirties.empty()) {
      socket_readable(sock, 8);
      continue;
    }
    const unsigned before_collapse = static_cast<unsigned>(dirties.size());
    collapse_dirties_for_send(&dirties, w, h);

    copies.clear();
    if (have_prev && (g_ptr_buttons & 1) && g_ptr_have_prev) {
      const int mdx = g_ptr_x - g_ptr_x_prev;
      const int mdy = g_ptr_y - g_ptr_y_prev;
      try_extract_copyrects(frame.data(), prev.data(), w, h, mdx, mdy, &dirties, &copies);
    }

    const bool full_keyframe =
        dirties.size() == 1 && dirties[0].x == 0 && dirties[0].y == 0 && dirties[0].w == w &&
        dirties[0].h == h;
    if (!full_keyframe) {
      split_rects_for_input_slices(&dirties, 256);
    }

    bool ok = true;
    size_t raw_this = 0;
    size_t wire_this = 0;
    DWORD enc_this = 0;
    DWORD send_this = 0;
    unsigned zlib_this = 0;
    unsigned raw_this_n = 0;
    unsigned copy_this = 0;
    const unsigned batch_n =
        static_cast<unsigned>(dirties.size()) + static_cast<unsigned>(copies.size());
    unsigned sent_this = 0;

    for (const CopyRect& cr : copies) {
      if (!drain_incoming(tls)) {
        ok = false;
        break;
      }
      if (!send_copy_rect(tls, frame_id, cr)) {
        logf("copyrect send failed - client gone?");
        ok = false;
        break;
      }
      ++g_copyrects_sent;
      ++copy_this;
      ++sent_rects;
      ++sent_this;
      ++frame_id;
      wire_this += kCopyRectPayloadSize;
    }
    if (!ok) {
      break;
    }

    for (const DirtyRect& r : dirties) {
      if (!rect_in_desk(w, h, r.x, r.y, r.w, r.h)) {
        logf("bad dirty rect %d,%d %dx%d desk=%dx%d - keyframe", r.x, r.y, r.w, r.h, w, h);
        have_prev = false;
        sent_this = 0;
        ok = true;
        break;
      }
      if (!drain_incoming(tls)) {
        ok = false;
        break;
      }
      SendRectStats st{};
      if (!send_video_rect(tls, frame_id, w, h, r.x, r.y, r.w, r.h, frame.data(), &raw_buf,
                           &send_buf, &st)) {
        logf("video send failed - client gone? id=%u rect=%dx%d", frame_id, r.w, r.h);
        ok = false;
        break;
      }
      raw_this += st.raw_bytes;
      wire_this += st.wire_bytes;
      enc_this += st.encode_ms;
      send_this += st.send_ms;
      if (st.codec == road_desk::replace::kVideoZlibBgra) {
        ++zlib_this;
      } else {
        ++raw_this_n;
      }
      ++sent_rects;
      ++sent_this;
      ++frame_id;
    }
    if (!ok) {
      break;
    }
    if (sent_this != batch_n) {
      have_prev = false;
      ++dropped_ticks;
      logf("partial/bad batch - force keyframe drop_ticks=%u", dropped_ticks);
      continue;
    }

    for (const CopyRect& cr : copies) {
      DirtyRect d{cr.dx, cr.dy, cr.w, cr.h};
      copy_rect_to_prev(prev.data(), frame.data(), w, h, d);
    }
    for (const DirtyRect& r : dirties) {
      copy_rect_to_prev(prev.data(), frame.data(), w, h, r);
    }
    have_prev = true;
    last_send_ms = GetTickCount();
    ++ticks_sent;
    sum_raw += raw_this;
    sum_wire += wire_this;
    sum_encode_ms += enc_this;
    sum_send_ms += send_this;
    zlib_rects += zlib_this;
    raw_rects += raw_this_n;

    const bool first = (ticks_sent == 1);
    const bool periodic = (last_send_ms - last_stat_ms >= kStatIntervalMs);
    if (first || periodic) {
      last_stat_ms = last_send_ms;
      const unsigned ratio =
          (raw_this > 0) ? static_cast<unsigned>((wire_this * 100u) / raw_this) : 0;
      const int ex_w = dirties.empty() ? (copies.empty() ? 0 : copies[0].w) : dirties[0].w;
      const int ex_h = dirties.empty() ? (copies.empty() ? 0 : copies[0].h) : dirties[0].h;
      logf(
          "tick=%u id=%u batch=%u(from %u) copy=%u raw=%u wire=%u ratio=%u%% zlib=%u "
          "enc_ms=%u send_ms=%u cap_ms=%u drop=%u in_ptr=%u ex=%dx%d",
          ticks_sent, frame_id - 1, batch_n, before_collapse, copy_this,
          static_cast<unsigned>(raw_this), static_cast<unsigned>(wire_this), ratio, zlib_this,
          enc_this, send_this, cap_ms, dropped_ticks, g_input_pointer, ex_w, ex_h);
    }
  }

  logf("session stats ticks=%u rects=%u zlib=%u raw=%u raw_B=%llu wire_B=%llu enc_ms=%llu "
       "send_ms=%llu drop=%u in_ptr=%u in_key=%u",
       ticks_sent, sent_rects, zlib_rects, raw_rects, static_cast<unsigned long long>(sum_raw),
       static_cast<unsigned long long>(sum_wire), static_cast<unsigned long long>(sum_encode_ms),
       static_cast<unsigned long long>(sum_send_ms), dropped_ticks, g_input_pointer, g_input_key);
  release_modifiers();
  road_desk::replace::log_set_mirror_stderr(true);
}

}  // namespace

int main(int argc, char** argv) {
  using road_desk::replace::CaptureMode;
  using road_desk::replace::parse_capture_mode;

  SetProcessDPIAware();
  road_desk::replace::log_open("replace_host.log");
  logf("boot build=cursor-m2");

  if (!wsa_init()) {
    logf("WSAStartup failed");
    road_desk::replace::log_close();
    return 1;
  }

  int port = 5902;
  std::string psk = "road-desk";
  CaptureMode capture_mode = CaptureMode::Auto;
  if (argc >= 2 && argv[1] && argv[1][0]) {
    port = std::atoi(argv[1]);
  }
  if (argc >= 3 && argv[2] && argv[2][0]) {
    psk = argv[2];
  }
  if (argc >= 4 && argv[3] && argv[3][0]) {
    capture_mode = parse_capture_mode(argv[3]);
  }
  {
    char* e = nullptr;
    size_t n = 0;
    if (_dupenv_s(&e, &n, "ROAD_DESK_PSK") == 0 && e) {
      psk = e;
      free(e);
    }
  }
  {
    char* e = nullptr;
    size_t n = 0;
    if (_dupenv_s(&e, &n, "ROAD_DESK_CAPTURE") == 0 && e) {
      capture_mode = parse_capture_mode(e);
      free(e);
    }
  }
  logf("capture mode=%s (auto|mirror|gdi; env ROAD_DESK_CAPTURE)",
       capture_mode == CaptureMode::Gdi      ? "gdi"
       : capture_mode == CaptureMode::Mirror ? "mirror"
                                            : "auto");

  if (!road_desk::session::authenticate_psk(psk, psk)) {
    logf("PSK required - set ROAD_DESK_PSK or pass [password]");
    return 1;
  }

  road_desk::media::tls::HostCredentials creds{};
  if (!road_desk::media::tls::create_self_signed_credentials(&creds)) {
    logf("create_self_signed_credentials failed");
    return 1;
  }
  logf("TLS fingerprint SHA-256: %s", creds.fingerprint_sha256_hex.c_str());

  SOCKET listen_sock = road_desk::media::tls::tcp_listen(port);
  if (listen_sock == INVALID_SOCKET) {
    logf("listen failed on %d (fail-fast)", port);
    road_desk::media::tls::free_host_credentials(&creds);
    return 1;
  }
  logf("listening TCP+TLS port %d (private mux, not RFB)", port);

  road_desk::session::SessionMutex session;

  for (;;) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listen_sock, &rfds);
    const int sel = select(0, &rfds, nullptr, nullptr, nullptr);
    if (sel <= 0) {
      logf("select failed (%d)", WSAGetLastError());
      break;
    }
    SOCKET tcp = road_desk::media::tls::tcp_accept(listen_sock);
    if (tcp == INVALID_SOCKET) {
      const int err = WSAGetLastError();
      if (err == WSAEWOULDBLOCK) {
        continue;
      }
      logf("accept failed (%d)", err);
      break;
    }
    if (!session.try_acquire()) {
      logf("reject: session busy");
      closesocket(tcp);
      continue;
    }

    road_desk::media::tls::TlsSession* tls =
        road_desk::media::tls::server_handshake(tcp, creds);
    if (!tls) {
      logf("TLS handshake failed");
      session.release();
      continue;
    }
    logf("TLS up - awaiting Control auth");
    serve_one(tls, psk, capture_mode);
    road_desk::media::tls::tls_close(tls);
    session.release();
    logf("session released - ready for next client");
  }

  closesocket(listen_sock);
  road_desk::media::tls::free_host_credentials(&creds);
  WSACleanup();
  road_desk::replace::log_close();
  return 0;
}
