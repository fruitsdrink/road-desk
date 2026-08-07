#pragma once

#include "mux_protocol.h"

#include "tls_schannel.h"

#include <cstdint>
#include <string>
#include <vector>

namespace road_desk::replace {

bool mux_write(road_desk::media::tls::TlsSession* tls, uint8_t channel,
               const void* payload, uint32_t len);

// Like mux_write, but calls yield(ctx) between TLS record chunks so another thread
// can briefly take the IO lock (e.g. inject drain/SendInput) during large video frames.
using MuxYieldFn = bool (*)(void* ctx);
bool mux_write_yield(road_desk::media::tls::TlsSession* tls, uint8_t channel,
                     const void* payload, uint32_t len, MuxYieldFn yield, void* yield_ctx);

// Reads one full frame. Returns false on disconnect / error / oversized.
bool mux_read(road_desk::media::tls::TlsSession* tls, uint8_t* channel_out,
              std::vector<uint8_t>* payload_out);

// Like mux_read, but calls idle(ctx) while waiting for socket data (e.g. flush Input).
// idle returning false aborts the read.
using MuxIdleFn = bool (*)(void* ctx);
bool mux_read_idle(road_desk::media::tls::TlsSession* tls, uint8_t* channel_out,
                   std::vector<uint8_t>* payload_out, MuxIdleFn idle, void* idle_ctx);

bool control_send_auth(road_desk::media::tls::TlsSession* tls, const std::string& password,
                       const std::string& session_id = std::string());
// session_role: kSessionRoleControl | kSessionRoleViewOnly (A5a).
// host_desktop_state: kHostDesktopConsole (default), Logon, Locked, None (S5).
bool control_send_auth_ok(road_desk::media::tls::TlsSession* tls, uint16_t width,
                          uint16_t height, const std::string& version = std::string(),
                          uint8_t session_role = kSessionRoleControl,
                          uint8_t host_desktop_state = kHostDesktopConsole);
bool control_send_auth_fail(road_desk::media::tls::TlsSession* tls, const std::string& reason);
bool control_send_session_role(road_desk::media::tls::TlsSession* tls, uint8_t session_role);

// Parse Auth payload. Optional trailing session_id (A2); old clients omit it.
bool parse_auth_password(const uint8_t* p, size_t n, std::string* password_out,
                         std::string* session_id_out = nullptr);

// Parse AuthOk: width/height required; version + session_role + host_desktop_state optional.
bool parse_auth_ok(const uint8_t* p, size_t n, uint16_t* width_out, uint16_t* height_out,
                   std::string* version_out = nullptr, uint8_t* session_role_out = nullptr,
                   uint8_t* host_desktop_state_out = nullptr);

}  // namespace road_desk::replace
