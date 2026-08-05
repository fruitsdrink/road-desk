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
  // Outside VNC display-port band (5900–5999) to avoid clashing with Tight/UltraVNC.
  int listen_port = 38471;
  std::string password = "road-desk";
  std::string desktop_name = "Road Desk";
  // Default true: private mux only on Schannel TLS. Mux has no plaintext path.
  bool require_tls = true;
  // Optional: pre-created cert store path (unused in MVP — host generates self-signed).
  std::string tls_cert_path;
  // Optional in-process session counter (Agent-owned). Shared control; not exclusive.
  session::SessionMutex* session_mutex = nullptr;

  // Host → control-plane audit (A2). Optional; never blocks media path long.
  struct AuditEvent {
    const char* phase = nullptr;             // opened | failed | closed
    const char* result = nullptr;            // ok | auth_fail | capacity_reject | ...
    const char* disconnect_reason = nullptr; // transport_lost | ...
    const char* session_id = nullptr;        // may be empty → reporter invents UUID
    const char* viewer_ip = nullptr;
  };
  using AuditFn = void (*)(void* user, const AuditEvent* ev);
  AuditFn audit_fn = nullptr;
  void* audit_user = nullptr;
};

// Host-side media plane (Mirror/GDI capture + TLS mux + input inject).
// listen()+serve() on the calling thread (Win7-friendly).
class MediaPlane {
 public:
  MediaPlane();
  ~MediaPlane();

  MediaPlane(const MediaPlane&) = delete;
  MediaPlane& operator=(const MediaPlane&) = delete;

  // listen() binds; serve() blocks in accept/session loop until request_stop().
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
  std::string host_port = "127.0.0.1:38471";
  std::string password = "road-desk";
  // Optional audit correlation id (UUID); sent after password in kCtrlAuth (A2).
  std::string audit_session_id;
  HWND notify_hwnd = nullptr;
  UINT resize_msg = WM_APP + 1;
  // Posted when the mux thread ends (transport loss). 0 → legacy WM_CLOSE.
  UINT disconnect_msg = 0;
  bool require_tls = true;
  // Debug only: skip fingerprint check (ROAD_DESK_TLS_INSECURE=1).
  bool tls_insecure = false;
  // Expected host cert SHA-256 (lowercase hex). Required when require_tls && !tls_insecure.
  std::string tls_fingerprint_sha256;
};

// Why the last start() / mux exit failed (for Viewer reconnect policy).
enum class MediaClientFail : uint8_t {
  None = 0,
  Transient = 1,  // TCP/TLS/network/timeout — safe to retry
  Auth = 2,       // PSK rejected — do not loop
  Config = 3,     // Missing fingerprint / bad host:port — do not loop
};

// Viewer-side media client (TLS mux connect + frame + input send).
// copy_frame_bgra includes composited software cursor.
class MediaClient {
 public:
  MediaClient();
  ~MediaClient();

  MediaClient(const MediaClient&) = delete;
  MediaClient& operator=(const MediaClient&) = delete;

  bool start(const MediaClientConfig& config);
  void stop();
  bool connected() const;
  MediaClientFail last_fail() const;
  // Agent version string received in the auth-ok handshake (empty until connected).
  std::string agent_version() const;
  // Update paint/resize/close notify target (e.g. after reparenting the session HWND).
  void set_notify_hwnd(HWND hwnd);

  bool copy_frame_bgra(std::vector<uint8_t>& out, int& width, int& height) const;
  // Desktop pixels only (no software cursor). Use with framebuffer_epoch for UI caches.
  bool copy_desktop_bgra(std::vector<uint8_t>& out, int& width, int& height) const;
  // Cheap size probe — never copies the framebuffer (mouse hit-test path).
  bool framebuffer_size(int* width, int* height) const;
  // Bumps when Video rects are applied; UI can skip desktop memcpy if unchanged.
  uint32_t framebuffer_epoch() const;
  // Composite current software cursor onto a desktop-sized BGRA buffer.
  void composite_software_cursor(std::vector<uint8_t>& inout, int width, int height) const;
  void send_pointer(int button_mask, int x, int y);
  bool send_vk(unsigned vk, bool down);
  void release_modifiers();
  // When false, copy_frame_bgra omits software cursor (mouse left the view / letterbox).
  void set_software_cursor_enabled(bool enabled);
  // Call from UI thread on WM_CLIPBOARDUPDATE (after AddClipboardFormatListener).
  void notify_clipboard_changed();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace road_desk::media
