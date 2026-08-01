// Mux media-plane host — TLS Video/Input; capture = Mirror dirties (default) or GDI.
// Ported from spikes/media-replace/src/replace_host.cpp.
// Single-thread session I/O (select + send) to avoid TLS read/write deadlock.

#include "media_plane.h"

#include "auth.h"
#include "cursor_capture.h"
#include "media_log.h"
#include "mirror_capture.h"
#include "mux.h"
#include "mux_clipboard.h"
#include "mux_file_xfer.h"
#include "mux_inject.h"
#include "mux_protocol.h"
#include "session_mutex.h"
#include "tls_schannel.h"

#include "mirror_client.h"

#include "miniz.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

bool send_video_rect(tls::TlsSession* tls, uint32_t frame_id, int desk_w, int desk_h, int x,
                     int y, int rw, int rh, const uint8_t* bgra_full,
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
  for (int row = 0; row < h; ++row) {
    const uint8_t* a = cur + (static_cast<size_t>(dy + row) * desk_w + dx) * 4u;
    const uint8_t* b = prev + (static_cast<size_t>(sy + row) * desk_w + sx) * 4u;
    if (std::memcmp(a, b, static_cast<size_t>(w) * 4u) != 0) {
      return false;
    }
  }
  return true;
}

void try_extract_copyrects(const uint8_t* cur, const uint8_t* prev, int desk_w, int desk_h,
                           int mdx, int mdy, std::vector<DirtyRect>* dirties,
                           std::vector<CopyRect>* copies) {
  copies->clear();
  if (!cur || !prev || !dirties || dirties->empty()) {
    return;
  }
  if (mdx > -4 && mdx < 4 && mdy > -4 && mdy < 4) {
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
int g_ptr_buttons = 0;
int g_ptr_buttons_prev = 0;
int g_ptr_x = 0;
int g_ptr_y = 0;
int g_ptr_x_prev = 0;
int g_ptr_y_prev = 0;
bool g_ptr_have_prev = false;
int g_validate_frames = 0;
uint32_t g_cursor_hash_sent = 0;
uint32_t g_copyrects_sent = 0;

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

bool maybe_send_cursor(tls::TlsSession* tls) {
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
    if ((g_ptr_buttons_prev & 1) != (g_ptr_buttons & 1)) {
      g_validate_frames = 8;
    }
    g_ptr_buttons_prev = g_ptr_buttons;
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

bool push_local_clipboard(tls::TlsSession* tls, HostClipSession* clip) {
  using namespace road_desk::replace;
  if (!tls || !clip || clip->sending_files) {
    return true;
  }
  const DWORD seq = GetClipboardSequenceNumber();
  if (seq == 0 || seq == clip->last_seq) {
    return true;
  }
  if (clip->echo.should_skip_seq(seq)) {
    clip->last_seq = seq;
    return true;
  }

  // Prefer files, then bitmap, then text (HDROP > DIB > TEXT).
  if (clipboard_has_hdrop()) {
    FileOffer offer;
    std::string err;
    const uint32_t xid = clip->next_xfer_id++;
    if (!clipboard_build_file_offer(xid, &offer, &err)) {
      logf("clipboard hdrop build failed: %s", err.c_str());
      clip->last_seq = seq;
      return true;
    }
    std::vector<uint8_t> payload;
    if (!build_clip_files_offer_payload(offer, &payload)) {
      logf("clipboard offer encode failed");
      clip->last_seq = seq;
      return true;
    }
    if (!mux_write(tls, kChannelClipboard, payload.data(),
                   static_cast<uint32_t>(payload.size()))) {
      return false;
    }
    clip->last_seq = seq;
    clip->sending_files = true;
    logf("clipboard files out id=%u entries=%u bytes=%llu", offer.transfer_id,
         static_cast<unsigned>(offer.entries.size()),
         static_cast<unsigned long long>(offer.total_file_bytes));
    const bool ok = file_xfer_send_all(
        tls, offer,
        [tls, clip]() { return pump_clip_drain(tls, clip); }, &err);
    clip->sending_files = false;
    if (!ok) {
      logf("clipboard files send failed: %s", err.c_str());
      send_file_abort(tls, offer.transfer_id);
      return err == "pump abort" ? false : true;
    }
    logf("clipboard files send done id=%u", offer.transfer_id);
    return true;
  }

  if (clipboard_has_dib()) {
    std::vector<uint8_t> dib;
    if (!clipboard_read_dib(&dib)) {
      logf("clipboard dib read failed");
      clip->last_seq = seq;
      return true;
    }
    std::vector<uint8_t> payload;
    if (!build_clip_bitmap_payload(dib, &payload)) {
      logf("clipboard bitmap encode failed / too large (%zu) - drop", dib.size());
      clip->last_seq = seq;
      return true;
    }
    if (!pump_clip_drain(tls, clip)) {
      return false;
    }
    if (!mux_write(tls, kChannelClipboard, payload.data(),
                   static_cast<uint32_t>(payload.size()))) {
      return false;
    }
    if (!pump_clip_drain(tls, clip)) {
      return false;
    }
    clip->last_seq = seq;
    logf("clipboard bitmap out raw=%zu wire=%zu", dib.size(), payload.size());
    return true;
  }

  if (clipboard_has_text()) {
    std::vector<uint8_t> utf16;
    if (!clipboard_read_text_utf16(&utf16)) {
      clip->last_seq = seq;
      return true;
    }
    const uint32_t dig = clipboard_text_digest(utf16.data(), utf16.size());
    if (dig == clip->echo.last_text_digest && clip->echo.should_skip_seq(seq)) {
      clip->last_seq = seq;
      return true;
    }
    std::vector<uint8_t> payload;
    if (!build_clip_text_payload(utf16, &payload)) {
      logf("clipboard text too large (%zu) - drop", utf16.size());
      clip->last_seq = seq;
      return true;
    }
    if (!mux_write(tls, kChannelClipboard, payload.data(),
                   static_cast<uint32_t>(payload.size()))) {
      return false;
    }
    clip->last_seq = seq;
    logf("clipboard text out %zu bytes", utf16.size());
    return true;
  }

  clip->last_seq = seq;
  return true;
}

void serve_one(tls::TlsSession* tls, const std::string& psk,
               road_desk::replace::CaptureMode capture_mode,
               const std::atomic<bool>* stop_requested) {
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

  SessionCapture cap(capture_mode);
  if (!cap.begin()) {
    logf("capture begin failed");
    control_send_auth_fail(tls, "capture begin failed");
    return;
  }
  if (!control_send_auth_ok(tls, width, height)) {
    logf("send AuthOk failed");
    return;
  }
  if (cap.using_mirror()) {
    logf("auth ok desktop=%ux%u capture=mirror device=%s - M2 cursor+CopyRect+zlib", width,
         height, cap.mirror_device());
  } else if (capture_mode != CaptureMode::Gdi) {
    logf("auth ok desktop=%ux%u capture=gdi (mirror attach failed; fallback) - G3", width,
         height);
  } else {
    logf("auth ok desktop=%ux%u capture=gdi - G3 cursor+CopyRect+zlib", width, height);
  }
  media_log_set_mirror_stderr(false);
  release_modifiers();
  g_input_pointer = 0;
  g_input_key = 0;
  g_ptr_have_prev = false;
  g_ptr_buttons_prev = 0;
  g_validate_frames = 0;
  g_cursor_hash_sent = 0;
  g_copyrects_sent = 0;

  HostClipSession clip;
  clip.last_seq = GetClipboardSequenceNumber();

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
  DWORD last_attach_scrub_ms = GetTickCount();
  constexpr DWORD kStatIntervalMs = 1000;
  constexpr DWORD kAttachScrubMs = 2000;

  for (;;) {
    if (stop_requested && stop_requested->load()) {
      logf("stop requested - ending session");
      break;
    }
    if (!drain_incoming(tls, &clip)) {
      logf("client gone (recv) sent_rects=%u drop_ticks=%u input_ptr=%u input_key=%u copy=%u",
           sent_rects, dropped_ticks, g_input_pointer, g_input_key, g_copyrects_sent);
      break;
    }
    if (!push_local_clipboard(tls, &clip)) {
      logf("clipboard push failed - client gone?");
      break;
    }

    const DWORD loop_now = GetTickCount();
    if (cap.using_mirror()) {
      if (loop_now - last_attach_scrub_ms >= kAttachScrubMs) {
        last_attach_scrub_ms = loop_now;
        rdm_scrub_foreign_attach_registry();
      }
    }

    const SOCKET sock = tls::tls_get_socket(tls);
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

    int w = 0;
    int h = 0;
    const bool force_validate = cap.using_mirror() && g_validate_frames > 0;
    const DWORD cap0 = GetTickCount();
    mirror_dirties.clear();
    if (!cap.capture(&frame, &w, &h, &mirror_dirties, force_validate)) {
      Sleep(20);
      continue;
    }
    const DWORD cap_ms = GetTickCount() - cap0;

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

    if (cap.using_mirror() && !force_validate) {
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
    if (force_validate && g_validate_frames > 0) {
      --g_validate_frames;
    }
    if (dirties.empty()) {
      socket_readable(sock, 8);
      continue;
    }
    const unsigned before_collapse = static_cast<unsigned>(dirties.size());
    collapse_dirties_for_send(&dirties, w, h);

    copies.clear();
    if (have_prev && !force_validate && (g_ptr_buttons & 1) && g_ptr_have_prev) {
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
      if (!drain_incoming(tls, &clip)) {
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
      if (!drain_incoming(tls, &clip)) {
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
          "enc_ms=%u send_ms=%u cap_ms=%u drop=%u in_ptr=%u in_key=%u ex=%dx%d",
          ticks_sent, frame_id - 1, batch_n, before_collapse, copy_this,
          static_cast<unsigned>(raw_this), static_cast<unsigned>(wire_this), ratio, zlib_this,
          enc_this, send_this, cap_ms, dropped_ticks, g_input_pointer, g_input_key, ex_w, ex_h);
    }
  }

  logf("session stats ticks=%u rects=%u zlib=%u raw=%u raw_B=%llu wire_B=%llu enc_ms=%llu "
       "send_ms=%llu drop=%u in_ptr=%u in_key=%u",
       ticks_sent, sent_rects, zlib_rects, raw_rects, static_cast<unsigned long long>(sum_raw),
       static_cast<unsigned long long>(sum_wire), static_cast<unsigned long long>(sum_encode_ms),
       static_cast<unsigned long long>(sum_send_ms), dropped_ticks, g_input_pointer, g_input_key);
  road_desk::replace::file_recv_abort(&clip.recv);
  release_modifiers();
  logf("session end: released modifiers/buttons");
  media_log_set_mirror_stderr(true);
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
  impl_->capture_mode = road_desk::replace::CaptureMode::Auto;
  {
    char* e = nullptr;
    size_t n = 0;
    if (_dupenv_s(&e, &n, "ROAD_DESK_CAPTURE") == 0 && e) {
      impl_->capture_mode = road_desk::replace::parse_capture_mode(e);
      free(e);
    }
  }
  logf("capture mode=%s (auto|mirror|gdi; env ROAD_DESK_CAPTURE)",
       impl_->capture_mode == road_desk::replace::CaptureMode::Gdi      ? "gdi"
       : impl_->capture_mode == road_desk::replace::CaptureMode::Mirror ? "mirror"
                                                                       : "auto");

  if (impl_->capture_mode != road_desk::replace::CaptureMode::Gdi) {
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
    logf("listen failed on %d (fail-fast)", config.listen_port);
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
  logf("listening TCP+TLS port %d (private mux, not RFB)", config.listen_port);
  return true;
}

void MediaPlane::serve() {
  if (!impl_ || impl_->listen_sock == INVALID_SOCKET) {
    return;
  }

  while (!impl_->stop_requested.load()) {
    SOCKET listen_sock = impl_->listen_sock;
    if (listen_sock == INVALID_SOCKET) {
      break;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listen_sock, &rfds);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200000;  // 200ms — wake to observe stop_requested
    const int sel = select(0, &rfds, nullptr, nullptr, &tv);
    if (sel < 0) {
      const int err = WSAGetLastError();
      if (impl_->stop_requested.load() || impl_->listen_sock == INVALID_SOCKET) {
        break;
      }
      logf("select failed (%d)", err);
      break;
    }
    if (sel == 0) {
      continue;
    }
    if (impl_->stop_requested.load() || impl_->listen_sock == INVALID_SOCKET) {
      break;
    }

    SOCKET tcp = tls::tcp_accept(impl_->listen_sock);
    if (tcp == INVALID_SOCKET) {
      const int err = WSAGetLastError();
      if (err == WSAEWOULDBLOCK) {
        continue;
      }
      if (impl_->stop_requested.load() || impl_->listen_sock == INVALID_SOCKET) {
        break;
      }
      logf("accept failed (%d)", err);
      break;
    }

    session::SessionMutex* mutex = impl_->session_mutex;
    if (mutex && !mutex->try_acquire()) {
      logf("reject: session busy");
      closesocket(tcp);
      continue;
    }

    tls::TlsSession* tls_sess = tls::server_handshake(tcp, impl_->tls_creds);
    if (!tls_sess) {
      logf("TLS handshake failed");
      if (mutex) {
        mutex->release();
      }
      continue;
    }
    logf("TLS up - awaiting Control auth");
    serve_one(tls_sess, impl_->password, impl_->capture_mode, &impl_->stop_requested);
    tls::tls_close(tls_sess);
    if (mutex) {
      mutex->release();
    }
    logf("session released - ready for next client");
  }

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
