#pragma once

#include "protocol.h"

#include "tls_schannel.h"

#include <cstdint>
#include <string>
#include <vector>

namespace road_desk::replace {

bool mux_write(road_desk::media::tls::TlsSession* tls, uint8_t channel,
               const void* payload, uint32_t len);

// Reads one full frame. Returns false on disconnect / error / oversized.
bool mux_read(road_desk::media::tls::TlsSession* tls, uint8_t* channel_out,
              std::vector<uint8_t>* payload_out);

// Like mux_read, but calls idle(ctx) while waiting for socket data (e.g. flush Input).
// idle returning false aborts the read.
using MuxIdleFn = bool (*)(void* ctx);
bool mux_read_idle(road_desk::media::tls::TlsSession* tls, uint8_t* channel_out,
                   std::vector<uint8_t>* payload_out, MuxIdleFn idle, void* idle_ctx);

bool control_send_auth(road_desk::media::tls::TlsSession* tls, const std::string& password);
bool control_send_auth_ok(road_desk::media::tls::TlsSession* tls, uint16_t width,
                          uint16_t height);
bool control_send_auth_fail(road_desk::media::tls::TlsSession* tls, const std::string& reason);

// Parse Auth payload (after type byte already consumed or include type).
bool parse_auth_password(const uint8_t* p, size_t n, std::string* password_out);

}  // namespace road_desk::replace
