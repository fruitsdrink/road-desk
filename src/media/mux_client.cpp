// Mux media client — replaces LibVNC viewer path (Track P cutover).
// Net thread owns TLS after start(); UI queues Input and reads framebuffer.

#include "media_plane.h"
#include "h264_mf.h"
#include "jpeg_encode.h"
#include "media_log.h"
#include "mux.h"
#include "mux_clipboard.h"
#include "mux_file_xfer.h"
#include "mux_protocol.h"
#include "proc_stats.h"
#include "tls_schannel.h"

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
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace road_desk::media {
namespace {

constexpr int kKeyQueueCap = 128;
constexpr char kLogTag[] = "media-client";

struct ClientState;
bool flush_input_queue(ClientState* st);

struct KeyEvent {
  uint16_t vk = 0;
  uint8_t flags = 0;  // kInputKeyFlagDown | kInputKeyFlagExtended
};

void logf(const char* fmt, ...) {
  char line[2048];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  media_logf(kLogTag, "%s", line);
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

void write_u32_le(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

// Same extended-VK set as mux_inject (MediaClient API has no extended flag).
bool is_extended_vk(unsigned vk) {
  switch (vk) {
    case VK_RMENU:
    case VK_RCONTROL:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_UP:
    case VK_RIGHT:
    case VK_DOWN:
    case VK_NUMLOCK:
    case VK_DIVIDE:
    case VK_SNAPSHOT:
    case VK_RWIN:
      return true;
    default:
      return false;
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

// Alpha-blend software cursor into a BGRA framebuffer (desktop coords).
void composite_cursor_bgra(std::vector<uint8_t>& fb, int fw, int fh, const uint8_t* cur,
                           int cw, int ch, int hot_x, int hot_y, int mx, int my) {
  if (!cur || fw <= 0 || fh <= 0 || cw <= 0 || ch <= 0 || mx < 0 || my < 0) {
    return;
  }
  if (fb.size() < static_cast<size_t>(fw) * fh * 4u) {
    return;
  }
  const int left = mx - hot_x;
  const int top = my - hot_y;
  for (int row = 0; row < ch; ++row) {
    const int dy = top + row;
    if (dy < 0 || dy >= fh) {
      continue;
    }
    for (int col = 0; col < cw; ++col) {
      const int dx = left + col;
      if (dx < 0 || dx >= fw) {
        continue;
      }
      const uint8_t* s = cur + (static_cast<size_t>(row) * cw + col) * 4u;
      const uint8_t a = s[3];
      if (a == 0) {
        continue;
      }
      uint8_t* d = fb.data() + (static_cast<size_t>(dy) * fw + dx) * 4u;
      if (a == 255) {
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        d[3] = 255;
      } else {
        const unsigned ia = 255u - a;
        d[0] = static_cast<uint8_t>((s[0] * a + d[0] * ia) / 255u);
        d[1] = static_cast<uint8_t>((s[1] * a + d[1] * ia) / 255u);
        d[2] = static_cast<uint8_t>((s[2] * a + d[2] * ia) / 255u);
        d[3] = 255;
      }
    }
  }
}

struct ClientState {
  MediaClientConfig cfg;
  tls::TlsSession* tls = nullptr;
  HANDLE thread = nullptr;
  volatile LONG stop = 0;
  std::atomic<bool> connected{false};
  MediaClientFail last_fail = MediaClientFail::None;
  bool wsa_started = false;

  CRITICAL_SECTION frame_lock{};
  CRITICAL_SECTION input_lock{};
  CRITICAL_SECTION cursor_lock{};
  bool locks_ready = false;

  std::vector<uint8_t> pixels;
  int width = 0;
  int height = 0;
  int desk_w = 0;
  int desk_h = 0;
  // Agent version from the auth-ok handshake (empty until connected).
  std::string agent_version;
  // A5a: Host AuthOk / SessionRole → view_only. Updated from net thread.
  std::atomic<bool> host_forces_view_only{false};
  std::atomic<uint8_t> host_desktop_state{0};  // S5: HostDesktopState
  uint32_t frame_id = 0;
  uint32_t frame_epoch = 0;
  std::vector<uint8_t> decode_buf;
  uint32_t rects_applied = 0;
  uint32_t zlib_ok = 0;
  uint32_t raw_ok = 0;
  uint32_t copy_ok = 0;
  uint32_t decode_fail = 0;
  uint64_t wire_bytes = 0;
  DWORD last_stat_ms = 0;

  // Software cursor (Cursor channel); composited in copy_frame_bgra.
  std::vector<uint8_t> cursor_bgra;
  int cursor_w = 0;
  int cursor_h = 0;
  int cursor_hot_x = 0;
  int cursor_hot_y = 0;
  bool cursor_hidden = true;
  bool cursor_have = false;
  int soft_mx = -1;
  int soft_my = -1;
  // Viewer sets false on WM_MOUSELEAVE / outside letterbox so overlay does not stick.
  bool software_cursor_enabled = true;
  uint32_t cursor_updates = 0;

  // Latest-pointer-wins + small key queue (UI -> net thread).
  bool ptr_pending = false;
  uint8_t ptr_buttons = 0;
  uint16_t ptr_x = 0;
  uint16_t ptr_y = 0;
  KeyEvent keys[kKeyQueueCap]{};
  int key_head = 0;
  int key_tail = 0;
  bool key_is_down[256]{};
  uint32_t input_flush_ok = 0;
  uint32_t input_coalesced = 0;
  uint32_t input_key_drop = 0;

  CRITICAL_SECTION clip_lock{};
  bool clip_dirty = false;
  road_desk::replace::ClipboardEchoGuard clip_echo;
  DWORD clip_last_seq = 0;
  uint32_t next_xfer_id = 1;
  bool sending_files = false;
  road_desk::replace::FileRecvState file_recv;
  std::vector<MediaAuditFileItem> pending_audit_files;
};

std::string wide_to_utf8(const std::wstring& w) {
  if (w.empty()) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) {
    return {};
  }
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring join_wpath(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) {
    return b;
  }
  if (b.empty()) {
    return a;
  }
  if (a.back() == L'\\' || a.back() == L'/') {
    return a + b;
  }
  return a + L'\\' + b;
}

// Top-level HDROP roots only — directories recorded as name+path, not recursive children.
void queue_audit_file_roots(ClientState* st, const road_desk::replace::FileOffer& offer, bool outbound,
                            const std::wstring* recv_stage_root) {
  using namespace road_desk::replace;
  if (!st) {
    return;
  }
  std::vector<MediaAuditFileItem> batch;
  batch.reserve(offer.root_names.size());
  for (const std::wstring& name : offer.root_names) {
    MediaAuditFileItem item;
    item.outbound = outbound;
    item.name = wide_to_utf8(name);
    item.path = item.name;
    for (const FileOfferEntry& e : offer.entries) {
      if (e.rel_path != name) {
        continue;
      }
      item.is_dir = (e.flags & kClipEntryFlagDir) != 0;
      if (outbound && !e.abs_path.empty()) {
        item.path = wide_to_utf8(e.abs_path);
      } else if (!outbound && recv_stage_root && !recv_stage_root->empty()) {
        item.path = wide_to_utf8(join_wpath(*recv_stage_root, name));
      } else if (!e.abs_path.empty()) {
        item.path = wide_to_utf8(e.abs_path);
      }
      break;
    }
    if (item.path.size() > 512) {
      item.path.resize(512);
    }
    if (item.name.size() > 256) {
      item.name.resize(256);
    }
    batch.push_back(std::move(item));
  }
  if (batch.empty()) {
    return;
  }
  EnterCriticalSection(&st->clip_lock);
  for (MediaAuditFileItem& it : batch) {
    st->pending_audit_files.push_back(std::move(it));
  }
  LeaveCriticalSection(&st->clip_lock);
}

void init_locks(ClientState* st) {
  if (!st->locks_ready) {
    InitializeCriticalSection(&st->frame_lock);
    InitializeCriticalSection(&st->input_lock);
    InitializeCriticalSection(&st->cursor_lock);
    InitializeCriticalSection(&st->clip_lock);
    st->locks_ready = true;
  }
}

void destroy_locks(ClientState* st) {
  if (st->locks_ready) {
    DeleteCriticalSection(&st->frame_lock);
    DeleteCriticalSection(&st->input_lock);
    DeleteCriticalSection(&st->cursor_lock);
    DeleteCriticalSection(&st->clip_lock);
    st->locks_ready = false;
  }
}

bool send_file_abort_client(tls::TlsSession* tls, uint32_t xfer_id) {
  using namespace road_desk::replace;
  uint8_t body[5];
  body[0] = kFileAbort;
  write_u32_le(body + 1, xfer_id);
  return mux_write(tls, kChannelFile, body, 5);
}

void post_audit_activity(ClientState* st, WPARAM kind, LPARAM detail = 0) {
  if (!st || !st->cfg.audit_activity_msg || !st->cfg.notify_hwnd) {
    return;
  }
  PostMessageW(st->cfg.notify_hwnd, st->cfg.audit_activity_msg, kind, detail);
}

bool flush_clipboard_out(ClientState* st) {
  using namespace road_desk::replace;
  if (!st->tls || st->sending_files) {
    return true;
  }

  bool dirty = false;
  EnterCriticalSection(&st->clip_lock);
  dirty = st->clip_dirty;
  st->clip_dirty = false;
  LeaveCriticalSection(&st->clip_lock);
  if (!dirty) {
    return true;
  }

  const DWORD seq = GetClipboardSequenceNumber();
  if (seq == 0 || seq == st->clip_last_seq) {
    return true;
  }
  if (st->clip_echo.should_skip_seq(seq)) {
    st->clip_last_seq = seq;
    return true;
  }

  if (clipboard_has_hdrop()) {
    FileOffer offer;
    std::string err;
    const uint32_t xid = st->next_xfer_id++;
    if (!clipboard_build_file_offer(xid, &offer, &err)) {
      logf("clipboard hdrop build failed: %s", err.c_str());
      st->clip_last_seq = seq;
      return true;
    }
    std::vector<uint8_t> payload;
    if (!build_clip_files_offer_payload(offer, &payload)) {
      logf("clipboard offer encode failed");
      st->clip_last_seq = seq;
      return true;
    }
    if (!mux_write(st->tls, kChannelClipboard, payload.data(),
                   static_cast<uint32_t>(payload.size()))) {
      return false;
    }
    st->clip_last_seq = seq;
    st->sending_files = true;
    logf("clipboard files out id=%u entries=%u", offer.transfer_id,
         static_cast<unsigned>(offer.entries.size()));
    const bool ok = file_xfer_send_all(
        st->tls, offer,
        [st]() {
          if (InterlockedCompareExchange(&st->stop, 0, 0) != 0) {
            return false;
          }
          return flush_input_queue(st);
        },
        &err);
    st->sending_files = false;
    if (!ok) {
      logf("clipboard files send failed: %s", err.c_str());
      send_file_abort_client(st->tls, offer.transfer_id);
      return err == "pump abort" ? false : true;
    }
    logf("clipboard files send done id=%u", offer.transfer_id);
    queue_audit_file_roots(st, offer, /*outbound=*/true, nullptr);
    post_audit_activity(st, 2, static_cast<LPARAM>(offer.root_names.empty()
                                                       ? offer.entries.size()
                                                       : offer.root_names.size()));
    return true;
  }

  if (clipboard_has_dib()) {
    std::vector<uint8_t> dib;
    if (!clipboard_read_dib(&dib)) {
      logf("clipboard dib read failed");
      st->clip_last_seq = seq;
      return true;
    }
    std::vector<uint8_t> payload;
    if (!build_clip_bitmap_payload(dib, &payload)) {
      logf("clipboard bitmap encode failed / too large (%zu) - drop", dib.size());
      st->clip_last_seq = seq;
      return true;
    }
    if (!flush_input_queue(st)) {
      return false;
    }
    if (!mux_write(st->tls, kChannelClipboard, payload.data(),
                   static_cast<uint32_t>(payload.size()))) {
      return false;
    }
    st->clip_last_seq = seq;
    logf("clipboard bitmap out raw=%zu wire=%zu", dib.size(), payload.size());
    post_audit_activity(st, 1);
    return true;
  }

  if (clipboard_has_text()) {
    std::vector<uint8_t> utf16;
    if (!clipboard_read_text_utf16(&utf16)) {
      st->clip_last_seq = seq;
      return true;
    }
    std::vector<uint8_t> payload;
    if (!build_clip_text_payload(utf16, &payload)) {
      logf("clipboard text too large (%zu) - drop", utf16.size());
      st->clip_last_seq = seq;
      return true;
    }
    if (!mux_write(st->tls, kChannelClipboard, payload.data(),
                   static_cast<uint32_t>(payload.size()))) {
      return false;
    }
    st->clip_last_seq = seq;
    logf("clipboard text out %zu bytes", utf16.size());
    post_audit_activity(st, 1);
    return true;
  }

  st->clip_last_seq = seq;
  return true;
}

bool handle_clipboard_payload(ClientState* st, const std::vector<uint8_t>& payload) {
  using namespace road_desk::replace;
  if (payload.empty()) {
    return true;
  }
  if (payload[0] == kClipText) {
    std::vector<uint8_t> utf16;
    if (!parse_clip_text_payload(payload.data(), payload.size(), &utf16)) {
      logf("clipboard text parse failed");
      return true;
    }
    if (clipboard_write_text_utf16(utf16.data(), utf16.size(), &st->clip_echo)) {
      st->clip_last_seq = st->clip_echo.ignore_seq;
      logf("clipboard text in %zu bytes", utf16.size());
      post_audit_activity(st, 1);
    }
    return true;
  }
  if (payload[0] == kClipBitmap) {
    std::vector<uint8_t> dib;
    if (!parse_clip_bitmap_payload(payload.data(), payload.size(), &dib)) {
      logf("clipboard bitmap parse failed");
      return true;
    }
    if (clipboard_write_dib(dib.data(), dib.size(), &st->clip_echo)) {
      st->clip_last_seq = st->clip_echo.ignore_seq;
      logf("clipboard bitmap in %zu bytes", dib.size());
      post_audit_activity(st, 1);
    }
    return true;
  }
  if (payload[0] == kClipFilesOffer) {
    FileOffer offer;
    if (!parse_clip_files_offer_payload(payload.data(), payload.size(), &offer)) {
      logf("clipboard files offer parse failed");
      return true;
    }
    if (st->file_recv.active) {
      send_file_abort_client(st->tls, st->file_recv.offer.transfer_id);
      file_recv_abort(&st->file_recv);
    }
    std::string err;
    if (!file_recv_begin(&st->file_recv, offer, &err)) {
      logf("clipboard files recv begin failed: %s", err.c_str());
      send_file_abort_client(st->tls, offer.transfer_id);
    } else {
      logf("clipboard files offer id=%u entries=%u", offer.transfer_id,
           static_cast<unsigned>(offer.entries.size()));
    }
    return true;
  }
  return true;
}

bool handle_file_payload(ClientState* st, const std::vector<uint8_t>& payload) {
  using namespace road_desk::replace;
  DWORD echo_seq = 0;
  std::string err;
  if (!file_recv_on_payload(&st->file_recv, payload.data(), payload.size(), &echo_seq, &err)) {
    logf("file recv failed: %s", err.empty() ? "error" : err.c_str());
    if (st->file_recv.offer.transfer_id) {
      send_file_abort_client(st->tls, st->file_recv.offer.transfer_id);
    }
    file_recv_abort(&st->file_recv);
    return true;
  }
  if (echo_seq != 0) {
    st->clip_echo.ignore_seq = echo_seq;
    st->clip_last_seq = echo_seq;
    logf("clipboard files published id=%u", st->file_recv.offer.transfer_id);
    queue_audit_file_roots(st, st->file_recv.offer, /*outbound=*/false, &st->file_recv.stage_root);
    post_audit_activity(st, 3,
                        static_cast<LPARAM>(st->file_recv.offer.root_names.empty()
                                                ? 1
                                                : st->file_recv.offer.root_names.size()));
  }
  return true;
}

void apply_cursor_payload(ClientState* st, const std::vector<uint8_t>& payload) {
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

  EnterCriticalSection(&st->cursor_lock);
  st->cursor_bgra.swap(bgra);
  st->cursor_w = static_cast<int>(w);
  st->cursor_h = static_cast<int>(h);
  st->cursor_hot_x = static_cast<int>(hot_x);
  st->cursor_hot_y = static_cast<int>(hot_y);
  st->cursor_hidden = hidden;
  st->cursor_have = !hidden && !st->cursor_bgra.empty();
  ++st->cursor_updates;
  LeaveCriticalSection(&st->cursor_lock);

  // Soft cursor rarely used (Host blanks OS cursors; Viewer draws local arrow). Avoid
  // full-window invalidate on cursor-shape packets — that flashed the whole Viewer.
  if (st->cfg.notify_hwnd && st->cfg.frame_dirty_msg && st->soft_mx >= 0 && st->soft_my >= 0) {
    const int cw = st->cursor_w > 0 ? st->cursor_w : 32;
    const int ch = st->cursor_h > 0 ? st->cursor_h : 32;
    int x = st->soft_mx - st->cursor_hot_x;
    int y = st->soft_my - st->cursor_hot_y;
    if (x < 0) {
      x = 0;
    }
    if (y < 0) {
      y = 0;
    }
    PostMessageW(st->cfg.notify_hwnd, st->cfg.frame_dirty_msg, MAKELONG(x, y), MAKELONG(cw, ch));
  }
}

bool flush_input_queue(ClientState* st) {
  using namespace road_desk::replace;
  if (!st->tls) {
    return false;
  }

  bool has_ptr = false;
  uint8_t buttons = 0;
  uint16_t x = 0;
  uint16_t y = 0;
  KeyEvent local_keys[kKeyQueueCap];
  int nkeys = 0;

  EnterCriticalSection(&st->input_lock);
  if (st->ptr_pending) {
    has_ptr = true;
    buttons = st->ptr_buttons;
    x = st->ptr_x;
    y = st->ptr_y;
    st->ptr_pending = false;
  }
  while (st->key_head != st->key_tail && nkeys < kKeyQueueCap) {
    local_keys[nkeys++] = st->keys[st->key_head];
    st->key_head = (st->key_head + 1) % kKeyQueueCap;
  }
  LeaveCriticalSection(&st->input_lock);

  if (has_ptr) {
    uint8_t body[kInputPointerSize];
    body[0] = kInputPointer;
    body[1] = buttons;
    write_u16_le(body + 2, x);
    write_u16_le(body + 4, y);
    if (!mux_write(st->tls, kChannelInput, body, kInputPointerSize)) {
      return false;
    }
    ++st->input_flush_ok;
  }
  for (int i = 0; i < nkeys; ++i) {
    uint8_t body[kInputKeySize];
    body[0] = kInputKey;
    body[1] = local_keys[i].flags;
    write_u16_le(body + 2, local_keys[i].vk);
    if (!mux_write(st->tls, kChannelInput, body, kInputKeySize)) {
      return false;
    }
    ++st->input_flush_ok;
  }
  return true;
}

bool ensure_fb_locked(ClientState* st) {
  if (!st->pixels.empty()) {
    return true;
  }
  if (st->desk_w <= 0 || st->desk_h <= 0) {
    return false;
  }
  st->width = st->desk_w;
  st->height = st->desk_h;
  st->pixels.assign(static_cast<size_t>(st->width) * st->height * 4u, 0);
  return true;
}

bool apply_video_payload(ClientState* st, const std::vector<uint8_t>& payload) {
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

  if (!flush_input_queue(st)) {
    return false;
  }

  bool need_paint = false;
  // Dirty notify rect (may expand beyond the encoded rect for CopyRect).
  uint16_t paint_x = x;
  uint16_t paint_y = y;
  uint16_t paint_w = rw;
  uint16_t paint_h = rh;
  if (codec == kVideoCopyRect) {
    if (payload.size() < kCopyRectPayloadSize) {
      return true;
    }
    const uint16_t sx = read_u16_le(payload.data() + 13);
    const uint16_t sy = read_u16_le(payload.data() + 15);
    st->wire_bytes += kCopyRectPayloadSize;
    EnterCriticalSection(&st->frame_lock);
    if (!ensure_fb_locked(st)) {
      LeaveCriticalSection(&st->frame_lock);
      return true;
    }
    if (x + rw <= st->width && y + rh <= st->height && sx + rw <= st->width &&
        sy + rh <= st->height) {
      std::vector<uint8_t> tmp(static_cast<size_t>(rw) * rh * 4u);
      for (int row = 0; row < rh; ++row) {
        const uint8_t* src =
            st->pixels.data() + (static_cast<size_t>(sy + row) * st->width + sx) * 4u;
        std::memcpy(tmp.data() + static_cast<size_t>(row) * rw * 4u, src,
                    static_cast<size_t>(rw) * 4u);
      }
      for (int row = 0; row < rh; ++row) {
        uint8_t* dst =
            st->pixels.data() + (static_cast<size_t>(y + row) * st->width + x) * 4u;
        std::memcpy(dst, tmp.data() + static_cast<size_t>(row) * rw * 4u,
                    static_cast<size_t>(rw) * 4u);
      }
      st->frame_id = fid;
      ++st->rects_applied;
      ++st->copy_ok;
      ++st->frame_epoch;
      need_paint = true;
      // Invalidate src∪dst so a paint between CopyRect and the exposed-area dirty
      // does not leave a ghost window at the old position.
      const int x0 = x < sx ? x : sx;
      const int y0 = y < sy ? y : sy;
      const int x1 = (x + rw > sx + rw) ? (x + rw) : (sx + rw);
      const int y1 = (y + rh > sy + rh) ? (y + rh) : (sy + rh);
      paint_x = static_cast<uint16_t>(x0);
      paint_y = static_cast<uint16_t>(y0);
      paint_w = static_cast<uint16_t>(x1 - x0);
      paint_h = static_cast<uint16_t>(y1 - y0);
    }
    LeaveCriticalSection(&st->frame_lock);
  } else if (codec == kVideoRawBgra || codec == kVideoZlibBgra || codec == kVideoJpeg ||
             codec == kVideoH264) {
    const size_t raw_bytes = static_cast<size_t>(rw) * rh * 4u;
    const uint8_t* wire = payload.data() + kVideoHeaderSize;
    const size_t wire_len = payload.size() - kVideoHeaderSize;
    const uint8_t* src = nullptr;
    st->wire_bytes += wire_len + kVideoHeaderSize;

    if (codec == kVideoRawBgra) {
      if (wire_len < raw_bytes) {
        ++st->decode_fail;
        return true;
      }
      src = wire;
      ++st->raw_ok;
    } else if (codec == kVideoJpeg) {
      if (!decode_jpeg_to_bgra(wire, wire_len, rw, rh, &st->decode_buf)) {
        ++st->decode_fail;
        return true;
      }
      src = st->decode_buf.data();
      ++st->zlib_ok;
    } else if (codec == kVideoH264) {
      if (!decode_h264_to_bgra(wire, wire_len, rw, rh, &st->decode_buf)) {
        ++st->decode_fail;
        return true;
      }
      src = st->decode_buf.data();
      ++st->zlib_ok;
    } else {
      st->decode_buf.resize(raw_bytes);
      mz_ulong out_len = static_cast<mz_ulong>(raw_bytes);
      if (mz_uncompress(st->decode_buf.data(), &out_len, wire, static_cast<mz_ulong>(wire_len)) !=
              MZ_OK ||
          out_len != raw_bytes) {
        ++st->decode_fail;
        return true;
      }
      src = st->decode_buf.data();
      ++st->zlib_ok;
    }

    EnterCriticalSection(&st->frame_lock);
    const bool full =
        (x == 0 && y == 0 && rw == st->desk_w && rh == st->desk_h && st->desk_w > 0);
    if (full) {
      st->width = rw;
      st->height = rh;
      st->pixels.assign(src, src + raw_bytes);
      need_paint = true;
    } else {
      if (!ensure_fb_locked(st)) {
        LeaveCriticalSection(&st->frame_lock);
        return true;
      }
      if (x + rw <= st->width && y + rh <= st->height) {
        for (int row = 0; row < rh; ++row) {
          uint8_t* dst =
              st->pixels.data() + (static_cast<size_t>(y + row) * st->width + x) * 4u;
          std::memcpy(dst, src + static_cast<size_t>(row) * rw * 4u, static_cast<size_t>(rw) * 4u);
        }
        need_paint = true;
      } else {
        LeaveCriticalSection(&st->frame_lock);
        return true;
      }
    }
    st->frame_id = fid;
    ++st->rects_applied;
    ++st->frame_epoch;
    LeaveCriticalSection(&st->frame_lock);
  } else {
    return true;
  }

  if (need_paint && st->cfg.notify_hwnd) {
    if (st->cfg.frame_dirty_msg) {
      PostMessageW(st->cfg.notify_hwnd, st->cfg.frame_dirty_msg, MAKELONG(paint_x, paint_y),
                   MAKELONG(paint_w, paint_h));
    } else {
      InvalidateRect(st->cfg.notify_hwnd, nullptr, FALSE);
    }
  }

  const DWORD now = GetTickCount();
  if (st->last_stat_ms == 0) {
    st->last_stat_ms = now;
  }
  if (fid == 0 || (now - st->last_stat_ms) >= 1000) {
    st->last_stat_ms = now;
    const ProcStats ps = sample_proc_stats();
    logf("recv id=%u codec=%u rect=%u,%u %ux%u applied=%u copy=%u cur=%u flush=%u wire_B=%llu cpu1=%.0f cpuN=%.0f mem=%llu mem%%=%.1f",
         fid, codec, x, y, rw, rh, st->rects_applied, st->copy_ok, st->cursor_updates,
         st->input_flush_ok, static_cast<unsigned long long>(st->wire_bytes), ps.cpu_one_core_pct,
         ps.cpu_machine_pct, static_cast<unsigned long long>(ps.ws_bytes >> 20), ps.mem_pct);
  }
  return true;
}

bool idle_flush_input(void* ctx) {
  auto* st = static_cast<ClientState*>(ctx);
  if (InterlockedCompareExchange(&st->stop, 0, 0) != 0) {
    return false;
  }
  if (!flush_input_queue(st)) {
    return false;
  }
  return flush_clipboard_out(st);
}

DWORD WINAPI net_thread(LPVOID param) {
  using namespace road_desk::replace;
  auto* st = static_cast<ClientState*>(param);
  st->clip_last_seq = GetClipboardSequenceNumber();
  std::vector<uint8_t> payload;
  while (InterlockedCompareExchange(&st->stop, 0, 0) == 0) {
    if (!flush_input_queue(st)) {
      logf("input flush failed - disconnect");
      break;
    }
    if (!flush_clipboard_out(st)) {
      logf("clipboard flush failed - disconnect");
      break;
    }
    const SOCKET sock = tls::tls_get_socket(st->tls);
    const int pending = tls::tls_pending(st->tls);
    if (pending < 0) {
      logf("tls_pending failed");
      break;
    }
    // During drag (LMB held), poll at 0ms so frames land immediately. Non-drag
    // keeps 5ms to avoid busy-spinning the net thread when the desktop is idle.
    const bool dragging = (st->ptr_buttons & 1) != 0;
    if (pending == 0 && !socket_readable(sock, dragging ? 0 : 5)) {
      continue;
    }
    uint8_t ch = 0;
    if (!mux_read_idle(st->tls, &ch, &payload, idle_flush_input, st)) {
      logf("recv ended");
      break;
    }
    if (!flush_input_queue(st)) {
      break;
    }
    if (ch == kChannelVideo) {
      if (!apply_video_payload(st, payload)) {
        break;
      }
    } else if (ch == kChannelCursor) {
      apply_cursor_payload(st, payload);
    } else if (ch == kChannelClipboard) {
      if (!handle_clipboard_payload(st, payload)) {
        break;
      }
    } else if (ch == kChannelFile) {
      if (!handle_file_payload(st, payload)) {
        break;
      }
    } else if (ch == kChannelControl && !payload.empty()) {
      if (payload[0] == kCtrlPong) {
        continue;
      }
      if (payload[0] == kCtrlSessionRole && payload.size() >= 2) {
        const bool view_only = (payload[1] == kSessionRoleViewOnly);
        st->host_forces_view_only.store(view_only);
        logf("session role → %s", view_only ? "view_only" : "control");
        if (st->cfg.notify_hwnd && st->cfg.session_role_msg) {
          PostMessageW(st->cfg.notify_hwnd, st->cfg.session_role_msg,
                       view_only ? 1 : 0, 0);
        }
        continue;
      }
    }
  }
  road_desk::replace::file_recv_abort(&st->file_recv);
  InterlockedExchange(&st->stop, 1);
  st->connected = false;
  st->last_fail = MediaClientFail::Transient;
  if (st->cfg.notify_hwnd) {
    const UINT msg = st->cfg.disconnect_msg ? st->cfg.disconnect_msg : WM_CLOSE;
    PostMessageW(st->cfg.notify_hwnd, msg, 0, 0);
  }
  return 0;
}

void queue_pointer(ClientState* st, int buttons, int x, int y) {
  EnterCriticalSection(&st->input_lock);
  if (st->ptr_pending) {
    ++st->input_coalesced;
  }
  st->ptr_pending = true;
  st->ptr_buttons = static_cast<uint8_t>(buttons & 0xff);
  st->ptr_x = static_cast<uint16_t>(x < 0 ? 0 : x);
  st->ptr_y = static_cast<uint16_t>(y < 0 ? 0 : y);
  LeaveCriticalSection(&st->input_lock);

  EnterCriticalSection(&st->cursor_lock);
  st->soft_mx = x;
  st->soft_my = y;
  LeaveCriticalSection(&st->cursor_lock);
}

void queue_key_flags(ClientState* st, unsigned vk, uint8_t flags) {
  using namespace road_desk::replace;
  if (vk == 0 || vk > 0xFE) {
    return;
  }
  EnterCriticalSection(&st->input_lock);
  const int next = (st->key_tail + 1) % kKeyQueueCap;
  if (next != st->key_head) {
    st->keys[st->key_tail].vk = static_cast<uint16_t>(vk);
    st->keys[st->key_tail].flags = flags;
    st->key_tail = next;
    if (flags & kInputKeyFlagDown) {
      st->key_is_down[vk] = true;
    } else {
      st->key_is_down[vk] = false;
    }
  } else {
    ++st->input_key_drop;
  }
  LeaveCriticalSection(&st->input_lock);
}

void queue_release_modifiers(ClientState* st) {
  using namespace road_desk::replace;
  EnterCriticalSection(&st->input_lock);
  for (unsigned vk = 1; vk < 256; ++vk) {
    if (!st->key_is_down[vk]) {
      continue;
    }
    st->key_is_down[vk] = false;
    const int next = (st->key_tail + 1) % kKeyQueueCap;
    if (next == st->key_head) {
      ++st->input_key_drop;
      continue;
    }
    st->keys[st->key_tail].vk = static_cast<uint16_t>(vk);
    st->keys[st->key_tail].flags = 0;  // up
    st->key_tail = next;
  }
  st->ptr_pending = true;
  st->ptr_buttons = 0;
  LeaveCriticalSection(&st->input_lock);
}

}  // namespace

struct MediaClient::Impl {
  ClientState state;
  bool log_owned = false;  // true if start() opened viewer.log (not viewer main)
};

MediaClient::MediaClient() : impl_(new Impl) {
  init_locks(&impl_->state);
}

MediaClient::~MediaClient() {
  stop();
  if (impl_) {
    destroy_locks(&impl_->state);
  }
  delete impl_;
  impl_ = nullptr;
}

bool MediaClient::start(const MediaClientConfig& config) {
  if (!impl_ || impl_->state.connected.load() || impl_->state.thread) {
    return false;
  }
  ClientState* st = &impl_->state;
  st->cfg = config;
  st->last_fail = MediaClientFail::None;
  st->host_forces_view_only.store(false);
  st->agent_version.clear();
  InterlockedExchange(&st->stop, 0);

  // Prefer viewer.log (opened by viewer main). Open it here if standalone.
  const bool already = media_log_is_open();
  media_log_open("viewer.log");
  impl_->log_owned = !already;
  logf("boot mux-client");

  if (!config.require_tls) {
    logf("mux client requires TLS (require_tls=false)");
    st->last_fail = MediaClientFail::Config;
    return false;
  }
  if (!config.tls_insecure && config.tls_fingerprint_sha256.empty()) {
    logf("TLS fingerprint required, or tls_insecure=true");
    st->last_fail = MediaClientFail::Config;
    return false;
  }

  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    logf("WSAStartup failed");
    st->last_fail = MediaClientFail::Transient;
    return false;
  }
  st->wsa_started = true;

  std::string host;
  int port = 0;
  if (!tls::parse_host_port(config.host_port, &host, &port)) {
    logf("bad host:port '%s'", config.host_port.c_str());
    WSACleanup();
    st->wsa_started = false;
    st->last_fail = MediaClientFail::Config;
    return false;
  }

  SOCKET tcp = tls::tcp_connect(host.c_str(), port);
  if (tcp == INVALID_SOCKET) {
    logf("TCP connect failed %s:%d", host.c_str(), port);
    WSACleanup();
    st->wsa_started = false;
    st->last_fail = MediaClientFail::Transient;
    return false;
  }

  st->tls = tls::client_handshake(tcp, config.tls_insecure, config.tls_fingerprint_sha256);
  if (!st->tls) {
    logf("TLS handshake failed");
    WSACleanup();
    st->wsa_started = false;
    st->last_fail = config.tls_fingerprint_sha256.empty() ? MediaClientFail::Transient
                                                          : MediaClientFail::Config;
    return false;
  }
  logf("TLS up peer_fp=%s", tls::peer_fingerprint_sha256(st->tls).c_str());

  using namespace road_desk::replace;
  if (!control_send_auth(st->tls, config.password, config.audit_session_id)) {
    logf("send auth failed");
    tls::tls_close(st->tls);
    st->tls = nullptr;
    WSACleanup();
    st->wsa_started = false;
    st->last_fail = MediaClientFail::Transient;
    return false;
  }

  uint8_t ch = 0;
  std::vector<uint8_t> payload;
  DWORD auth_deadline = GetTickCount() + 8000;
  auto auth_idle = [](void* ctx) -> bool {
    return GetTickCount() < *static_cast<DWORD*>(ctx);
  };
  if (!mux_read_idle(st->tls, &ch, &payload, auth_idle, &auth_deadline) ||
      ch != kChannelControl || payload.empty()) {
    logf("no Control reply (auth timeout or disconnect)");
    tls::tls_close(st->tls);
    st->tls = nullptr;
    WSACleanup();
    st->wsa_started = false;
    st->last_fail = MediaClientFail::Transient;
    return false;
  }
  if (payload[0] == kCtrlAuthFail) {
    logf("auth rejected");
    tls::tls_close(st->tls);
    st->tls = nullptr;
    WSACleanup();
    st->wsa_started = false;
    st->last_fail = MediaClientFail::Auth;
    return false;
  }
  if (payload[0] != kCtrlAuthOk || payload.size() < 5) {
    logf("unexpected control");
    tls::tls_close(st->tls);
    st->tls = nullptr;
    WSACleanup();
    st->wsa_started = false;
    st->last_fail = MediaClientFail::Transient;
    return false;
  }

  uint16_t aw = 0;
  uint16_t ah = 0;
  uint8_t role = kSessionRoleControl;
  uint8_t host_st = kHostDesktopConsole;  // S5
  if (!parse_auth_ok(payload.data(), payload.size(), &aw, &ah, &st->agent_version, &role, &host_st)) {
    logf("AuthOk parse failed");
    tls::tls_close(st->tls);
    st->tls = nullptr;
    WSACleanup();
    st->wsa_started = false;
    st->last_fail = MediaClientFail::Transient;
    return false;
  }
  st->desk_w = aw;
  st->desk_h = ah;
  st->host_forces_view_only.store(role == kSessionRoleViewOnly);
  st->host_desktop_state.store(host_st);
  logf("auth ok desktop=%dx%d agent=%s role=%s host_st=%u", st->desk_w, st->desk_h,
       st->agent_version.empty() ? "-" : st->agent_version.c_str(),
       st->host_forces_view_only.load() ? "view_only" : "control",
       static_cast<unsigned>(host_st));

  if (st->cfg.notify_hwnd && st->cfg.resize_msg) {
    PostMessageW(st->cfg.notify_hwnd, st->cfg.resize_msg, static_cast<WPARAM>(st->desk_w),
                 static_cast<LPARAM>(st->desk_h));
  }

  st->thread = CreateThread(nullptr, 0, net_thread, st, 0, nullptr);
  if (!st->thread) {
    logf("net thread failed");
    tls::tls_close(st->tls);
    st->tls = nullptr;
    WSACleanup();
    st->wsa_started = false;
    st->last_fail = MediaClientFail::Transient;
    return false;
  }

  st->connected = true;
  st->last_fail = MediaClientFail::None;
  return true;
}

void MediaClient::stop() {
  if (!impl_) {
    return;
  }
  ClientState* st = &impl_->state;
  InterlockedExchange(&st->stop, 1);
  if (st->tls) {
    tls::tls_close(st->tls);
    st->tls = nullptr;
  }
  if (st->thread) {
    WaitForSingleObject(st->thread, 15000);
    CloseHandle(st->thread);
    st->thread = nullptr;
  }
  st->connected = false;

  EnterCriticalSection(&st->cursor_lock);
  st->cursor_bgra.clear();
  st->cursor_have = false;
  st->cursor_hidden = true;
  LeaveCriticalSection(&st->cursor_lock);

  EnterCriticalSection(&st->frame_lock);
  st->pixels.clear();
  st->width = 0;
  st->height = 0;
  LeaveCriticalSection(&st->frame_lock);

  if (st->wsa_started) {
    WSACleanup();
    st->wsa_started = false;
  }
    logf("stopped applied=%u copy=%u cur=%u flush=%u wire_B=%llu", st->rects_applied, st->copy_ok,
         st->cursor_updates, st->input_flush_ok, static_cast<unsigned long long>(st->wire_bytes));
  if (impl_->log_owned) {
    media_log_close();
    impl_->log_owned = false;
  }
}

bool MediaClient::connected() const {
  return impl_ && impl_->state.connected.load() &&
         InterlockedCompareExchange(&impl_->state.stop, 0, 0) == 0;
}

MediaClientFail MediaClient::last_fail() const {
  return impl_ ? impl_->state.last_fail : MediaClientFail::None;
}

std::string MediaClient::agent_version() const {
  return impl_ ? impl_->state.agent_version : std::string();
}

bool MediaClient::host_forces_view_only() const {
  return impl_ && impl_->state.host_forces_view_only.load();
}

uint8_t MediaClient::host_desktop_state() const {
  return impl_ ? impl_->state.host_desktop_state.load() : 0;
}

bool MediaClient::copy_desktop_bgra(std::vector<uint8_t>& out, int& width, int& height) const {
  if (!impl_) {
    return false;
  }
  ClientState* st = &impl_->state;
  EnterCriticalSection(&st->frame_lock);
  width = st->width;
  height = st->height;
  out = st->pixels;
  LeaveCriticalSection(&st->frame_lock);
  return width > 0 && height > 0 && !out.empty();
}

bool MediaClient::framebuffer_size(int* width, int* height) const {
  if (!impl_ || !width || !height) {
    return false;
  }
  ClientState* st = &impl_->state;
  EnterCriticalSection(&st->frame_lock);
  *width = st->width;
  *height = st->height;
  LeaveCriticalSection(&st->frame_lock);
  return *width > 0 && *height > 0;
}

uint32_t MediaClient::framebuffer_epoch() const {
  if (!impl_) {
    return 0;
  }
  ClientState* st = &impl_->state;
  EnterCriticalSection(&st->frame_lock);
  const uint32_t e = st->frame_epoch;
  LeaveCriticalSection(&st->frame_lock);
  return e;
}

void MediaClient::composite_software_cursor(std::vector<uint8_t>& inout, int width,
                                            int height) const {
  if (!impl_ || width <= 0 || height <= 0 ||
      inout.size() < static_cast<size_t>(width) * height * 4u) {
    return;
  }
  ClientState* st = &impl_->state;
  std::vector<uint8_t> cur;
  int cw = 0;
  int ch = 0;
  int hx = 0;
  int hy = 0;
  int mx = -1;
  int my = -1;
  bool draw = false;
  EnterCriticalSection(&st->cursor_lock);
  mx = st->soft_mx;
  my = st->soft_my;
  if (st->software_cursor_enabled && st->cursor_have && !st->cursor_hidden && mx >= 0 &&
      my >= 0 && !st->cursor_bgra.empty()) {
    cur = st->cursor_bgra;
    cw = st->cursor_w;
    ch = st->cursor_h;
    hx = st->cursor_hot_x;
    hy = st->cursor_hot_y;
    draw = true;
  }
  LeaveCriticalSection(&st->cursor_lock);
  if (draw) {
    composite_cursor_bgra(inout, width, height, cur.data(), cw, ch, hx, hy, mx, my);
  }
}

bool MediaClient::copy_frame_bgra(std::vector<uint8_t>& out, int& width, int& height) const {
  if (!copy_desktop_bgra(out, width, height)) {
    return false;
  }
  composite_software_cursor(out, width, height);
  return true;
}

void MediaClient::send_pointer(int button_mask, int x, int y) {
  if (!impl_ || !impl_->state.connected.load()) {
    return;
  }
  queue_pointer(&impl_->state, button_mask, x, y);
}

bool MediaClient::send_vk(unsigned vk, bool down) {
  using namespace road_desk::replace;
  if (!impl_ || !impl_->state.connected.load()) {
    return false;
  }
  if (vk == 0 || vk > 0xFE) {
    return false;
  }
  // Drop auto-repeat KEYDOWNs when already down (Host SendInput repeats poorly).
  if (down) {
    EnterCriticalSection(&impl_->state.input_lock);
    const bool already = impl_->state.key_is_down[vk];
    LeaveCriticalSection(&impl_->state.input_lock);
    if (already) {
      return true;
    }
  }
  uint8_t flags = down ? kInputKeyFlagDown : 0;
  if (is_extended_vk(vk)) {
    flags |= kInputKeyFlagExtended;
  }
  queue_key_flags(&impl_->state, vk, flags);
  return true;
}

void MediaClient::release_modifiers() {
  if (!impl_ || !impl_->state.connected.load()) {
    return;
  }
  queue_release_modifiers(&impl_->state);
}

void MediaClient::set_software_cursor_enabled(bool enabled) {
  if (!impl_) {
    return;
  }
  ClientState* st = &impl_->state;
  EnterCriticalSection(&st->cursor_lock);
  st->software_cursor_enabled = enabled;
  if (!enabled) {
    st->soft_mx = -1;
    st->soft_my = -1;
  }
  LeaveCriticalSection(&st->cursor_lock);
  if (st->cfg.notify_hwnd) {
    InvalidateRect(st->cfg.notify_hwnd, nullptr, FALSE);
  }
}

void MediaClient::notify_clipboard_changed() {
  if (!impl_ || !impl_->state.connected.load()) {
    return;
  }
  EnterCriticalSection(&impl_->state.clip_lock);
  impl_->state.clip_dirty = true;
  LeaveCriticalSection(&impl_->state.clip_lock);
}

std::vector<MediaAuditFileItem> MediaClient::take_pending_audit_files() {
  std::vector<MediaAuditFileItem> out;
  if (!impl_) {
    return out;
  }
  EnterCriticalSection(&impl_->state.clip_lock);
  out.swap(impl_->state.pending_audit_files);
  LeaveCriticalSection(&impl_->state.clip_lock);
  return out;
}

void MediaClient::set_notify_hwnd(HWND hwnd) {
  if (!impl_) {
    return;
  }
  impl_->state.cfg.notify_hwnd = hwnd;
}

}  // namespace road_desk::media
