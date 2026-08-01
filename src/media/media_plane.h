#pragma once

// Replaceable media-plane boundary. Control-plane code must not include RFB/LibVNC.

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace road_desk::session {
class SessionMutex;
}

namespace road_desk::media {

struct MediaPlaneConfig {
  int listen_port = 5900;
  std::string password = "road-desk";
  std::string desktop_name = "Road Desk";
  // Default true: RFB only on Schannel TLS. Set false only for local debug
  // (e.g. ROAD_DESK_ALLOW_PLAINTEXT=1).
  bool require_tls = true;
  // Optional: pre-created cert store path (unused in MVP — host generates self-signed).
  std::string tls_cert_path;
  // In-process session mutex (Agent-owned). Not cross-process.
  session::SessionMutex* session_mutex = nullptr;
};

// Host-side media plane (capture + RFB listen + input inject).
// LibVNC must run on the calling thread — use run() from agent main (like spike_host).
class MediaPlane {
 public:
  MediaPlane();
  ~MediaPlane();

  MediaPlane(const MediaPlane&) = delete;
  MediaPlane& operator=(const MediaPlane&) = delete;

  // Same calling thread for both (LibVNC/Win7). listen() binds; serve() blocks in the event loop.
  bool listen(const MediaPlaneConfig& config);
  void serve();
  void request_stop();
  bool running() const;
  int bound_port() const;
  // SHA-256 fingerprint of the host TLS cert (lowercase hex); empty if plaintext.
  std::string tls_fingerprint_sha256() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

struct MediaClientConfig {
  std::string host_port = "127.0.0.1:5900";
  std::string password = "road-desk";
  HWND notify_hwnd = nullptr;
  UINT resize_msg = WM_APP + 1;
  bool require_tls = true;
  // Debug only: skip fingerprint check (ROAD_DESK_TLS_INSECURE=1).
  bool tls_insecure = false;
  // Expected host cert SHA-256 (lowercase hex). Required when require_tls && !tls_insecure.
  std::string tls_fingerprint_sha256;
};

// Viewer-side media client (RFB connect + frame + input send).
class MediaClient {
 public:
  MediaClient();
  ~MediaClient();

  MediaClient(const MediaClient&) = delete;
  MediaClient& operator=(const MediaClient&) = delete;

  bool start(const MediaClientConfig& config);
  void stop();
  bool connected() const;

  bool copy_frame_bgra(std::vector<uint8_t>& out, int& width, int& height) const;
  void send_pointer(int button_mask, int x, int y);
  bool send_vk(unsigned vk, bool down);
  void release_modifiers();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace road_desk::media
