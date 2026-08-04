#include "mux.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>

#include <cstring>

namespace road_desk::replace {
namespace {

bool tls_read_full(road_desk::media::tls::TlsSession* tls, void* buf, int len) {
  auto* p = static_cast<uint8_t*>(buf);
  int got = 0;
  while (got < len) {
    const int n = road_desk::media::tls::tls_read(tls, p + got, len - got);
    if (n <= 0) {
      return false;
    }
    got += n;
  }
  return true;
}

bool tls_write_full(road_desk::media::tls::TlsSession* tls, const void* buf, int len) {
  auto* p = static_cast<const uint8_t*>(buf);
  int sent = 0;
  while (sent < len) {
    const int n = road_desk::media::tls::tls_write(tls, p + sent, len - sent);
    if (n <= 0) {
      return false;
    }
    sent += n;
  }
  return true;
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

uint32_t read_u32_le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

bool mux_write(road_desk::media::tls::TlsSession* tls, uint8_t channel, const void* payload,
               uint32_t len) {
  return mux_write_yield(tls, channel, payload, len, nullptr, nullptr);
}

bool mux_write_yield(road_desk::media::tls::TlsSession* tls, uint8_t channel,
                     const void* payload, uint32_t len, MuxYieldFn yield, void* yield_ctx) {
  if (!tls || len > kMaxPayloadLen) {
    return false;
  }
  uint8_t hdr[5];
  hdr[0] = channel;
  write_u32_le(hdr + 1, len);
  if (!tls_write_full(tls, hdr, 5)) {
    return false;
  }
  if (len == 0) {
    return true;
  }
  auto* p = static_cast<const uint8_t*>(payload);
  uint32_t sent = 0;
  // Small chunks so inject can drain between TLS records (cursor/window lag).
  constexpr uint32_t kYieldEvery = 4u * 1024u;
  while (sent < len) {
    if (yield && sent > 0) {
      if (!yield(yield_ctx)) {
        return false;
      }
    }
    const uint32_t chunk = (len - sent > kYieldEvery) ? kYieldEvery : (len - sent);
    if (!tls_write_full(tls, p + sent, static_cast<int>(chunk))) {
      return false;
    }
    sent += chunk;
  }
  return true;
}

bool mux_read(road_desk::media::tls::TlsSession* tls, uint8_t* channel_out,
              std::vector<uint8_t>* payload_out) {
  return mux_read_idle(tls, channel_out, payload_out, nullptr, nullptr);
}

bool mux_read_idle(road_desk::media::tls::TlsSession* tls, uint8_t* channel_out,
                   std::vector<uint8_t>* payload_out, MuxIdleFn idle, void* idle_ctx) {
  if (!tls || !channel_out || !payload_out) {
    return false;
  }

  auto pump = [&](void* buf, int len) -> bool {
    auto* p = static_cast<uint8_t*>(buf);
    int got = 0;
    while (got < len) {
      if (idle) {
        if (!idle(idle_ctx)) {
          return false;
        }
      }
      const SOCKET sock = road_desk::media::tls::tls_get_socket(tls);
      if (sock == INVALID_SOCKET) {
        return false;
      }
      const int pending = road_desk::media::tls::tls_pending(tls);
      if (pending < 0) {
        return false;
      }
      bool readable = pending > 0;
      if (!readable) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 2000;  // 2ms — keep pumping Input while waiting
        const int sel = select(0, &rfds, nullptr, nullptr, &tv);
        if (sel < 0) {
          return false;
        }
        readable = (sel > 0 && FD_ISSET(sock, &rfds));
      }
      if (!readable) {
        continue;
      }
      const int n = road_desk::media::tls::tls_read(tls, p + got, len - got);
      if (n <= 0) {
        return false;
      }
      got += n;
    }
    return true;
  };

  uint8_t hdr[5];
  if (!pump(hdr, 5)) {
    return false;
  }
  *channel_out = hdr[0];
  const uint32_t len = read_u32_le(hdr + 1);
  if (len > kMaxPayloadLen) {
    return false;
  }
  payload_out->assign(len, 0);
  if (len == 0) {
    return true;
  }
  return pump(payload_out->data(), static_cast<int>(len));
}

bool control_send_auth(road_desk::media::tls::TlsSession* tls, const std::string& password) {
  if (password.size() > 1024) {
    return false;
  }
  std::vector<uint8_t> body(1 + 2 + password.size());
  body[0] = kCtrlAuth;
  write_u16_le(body.data() + 1, static_cast<uint16_t>(password.size()));
  if (!password.empty()) {
    std::memcpy(body.data() + 3, password.data(), password.size());
  }
  return mux_write(tls, kChannelControl, body.data(), static_cast<uint32_t>(body.size()));
}

bool control_send_auth_ok(road_desk::media::tls::TlsSession* tls, uint16_t width,
                          uint16_t height) {
  uint8_t body[5];
  body[0] = kCtrlAuthOk;
  write_u16_le(body + 1, width);
  write_u16_le(body + 3, height);
  return mux_write(tls, kChannelControl, body, 5);
}

bool control_send_auth_fail(road_desk::media::tls::TlsSession* tls, const std::string& reason) {
  if (reason.size() > 1024) {
    return false;
  }
  std::vector<uint8_t> body(1 + 2 + reason.size());
  body[0] = kCtrlAuthFail;
  write_u16_le(body.data() + 1, static_cast<uint16_t>(reason.size()));
  if (!reason.empty()) {
    std::memcpy(body.data() + 3, reason.data(), reason.size());
  }
  return mux_write(tls, kChannelControl, body.data(), static_cast<uint32_t>(body.size()));
}

bool parse_auth_password(const uint8_t* p, size_t n, std::string* password_out) {
  if (!p || !password_out || n < 3 || p[0] != kCtrlAuth) {
    return false;
  }
  const uint16_t plen = read_u16_le(p + 1);
  if (n < 3u + plen) {
    return false;
  }
  password_out->assign(reinterpret_cast<const char*>(p + 3), plen);
  return true;
}

}  // namespace road_desk::replace
