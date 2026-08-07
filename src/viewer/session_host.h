#pragma once

#include "connect_config.h"
#include "media_plane.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace road_desk::viewer {
LRESULT CALLBACK SessionWndProc(HWND, UINT, WPARAM, LPARAM);
}

namespace road_desk::viewer {

constexpr UINT WM_MEDIA_RESIZE = WM_APP + 1;
// Posted to owner when session HWND is closing (disconnect / user close).
constexpr UINT WM_SESSION_CLOSED = WM_APP + 2;
// Posted to the console when session metadata (agent version) is available.
constexpr UINT WM_SESSION_META = WM_APP + 3;
// Mux transport ended; SessionHost decides reconnect vs destroy.
constexpr UINT WM_MEDIA_TRANSPORT_LOST = WM_APP + 4;
// MediaClient: clipboard (wParam=1) or file transfer used this session.
// File: wParam=2 Viewer→Host, wParam=3 Host→Viewer; lParam = entry count (no filenames).
constexpr UINT WM_MEDIA_AUDIT_ACTIVITY = WM_APP + 5;
// Host changed session role mid-session (A5a promote). wParam: 0=control, 1=view_only.
constexpr UINT WM_MEDIA_SESSION_ROLE = WM_APP + 6;
// Host/media dirty rect in framebuffer coords. wParam=MAKELONG(x,y) lParam=MAKELONG(w,h).
constexpr UINT WM_MEDIA_FRAME_DIRTY = WM_APP + 7;

class SessionHost {
 public:
  using ClosedFn = std::function<void(SessionHost*)>;

  SessionHost();
  ~SessionHost();

  SessionHost(const SessionHost&) = delete;
  SessionHost& operator=(const SessionHost&) = delete;

  static bool register_class(HINSTANCE instance);

  // Create child (docked) or top-level (floating) host window and start MediaClient.
  // auto_reconnect: console tabs retry on transport loss; CLI direct session stays false.
  bool open(HINSTANCE instance, HWND parent_or_null, const std::wstring& title,
            const ConnectDefaults& connect, ClosedFn on_closed, bool auto_reconnect = false);

  // Gateway audit (A1). Call before open() when directory reporting is enabled.
  void set_audit_session(const std::string& session_id, const std::string& agent_id,
                         const std::string& agent_name_utf8, const std::string& agent_endpoint);
  const std::string& audit_session_id() const { return audit_session_id_; }

  void close();
  void clear_closed_handler() { on_closed_ = nullptr; }
  bool connected() const;
  bool reconnecting() const { return reconnecting_; }
  int reconnect_attempt() const { return reconnect_attempt_; }
  // Short status for the console status bar (empty when idle/connected).
  std::wstring status_text() const;
  // Version reported by the connected host agent (empty until connected).
  std::string agent_version() const { return client_.agent_version(); }
  HWND hwnd() const { return hwnd_; }
  bool floating() const { return floating_; }
  const std::wstring& title() const { return title_; }
  int device_id() const { return device_id_; }
  void set_device_id(int id) { device_id_ = id; }

  // Reparent between console container and independent top-level window.
  bool detach_to_floating(HINSTANCE instance);
  bool attach_to_parent(HWND parent);

  // Keyboard routing (LL hook / focus).
  bool wants_keyboard() const;
  bool send_vk(unsigned vk, bool down);
  void release_modifiers();

  // View-only: freeze keyboard/mouse/clipboard forwarding; video stays live.
  void set_view_only(bool on);
  bool view_only() const { return view_only_; }
  // Host assigned view-only at AuthOk (another Viewer holds control). Local toggle cannot clear.
  bool host_forced_view_only() const { return host_forced_view_only_; }

  // Fullscreen the session window (docked sessions detach first). Esc exits.
  bool toggle_fullscreen();
  bool fullscreen() const { return fullscreen_; }

  // 1:1 display mode (default) vs fit-to-window. 1:1 preserves ClearType pixel-for-pixel;
  // scrollbars appear when the framebuffer is larger than the client area.
  bool fit_to_window() const { return fit_to_window_; }
  void set_fit_to_window(bool on);

  // Save the current remote framebuffer (with cursor) as PNG.
  bool save_screenshot(const std::wstring& path);

  static SessionHost* from_hwnd(HWND hwnd);
  static void install_keyboard_hook(HINSTANCE instance);
  static void uninstall_keyboard_hook();
  static void set_keyboard_target(SessionHost* host);

 private:
  friend LRESULT CALLBACK SessionWndProc(HWND, UINT, WPARAM, LPARAM);

  void paint();
  void release_backbuffer();
  bool ensure_backbuffer(HDC hdc, int cw, int ch);
  void on_destroy();
  void notify_chrome_changed();
  void on_transport_lost();
  void schedule_reconnect();
  void try_reconnect();
  void stop_reconnect(const wchar_t* status);
  road_desk::media::MediaClientConfig make_client_config() const;
  int reconnect_delay_ms() const;
  void audit_emit(const char* phase, const char* result, const char* disconnect_reason,
                  bool flag_clipboard = false, bool flag_file = false);
  void apply_host_session_role();
  void update_scrollbars();

  road_desk::media::MediaClient client_;
  ConnectDefaults connect_{};
  HWND hwnd_ = nullptr;
  bool floating_ = false;
  bool started_ = false;
  bool auto_reconnect_ = false;
  bool user_closing_ = false;
  bool reconnecting_ = false;
  bool reconnect_gave_up_ = false;
  int reconnect_attempt_ = 0;
  std::wstring status_override_;
  std::wstring title_;
  int device_id_ = -1;
  bool view_only_ = false;
  bool host_forced_view_only_ = false;
  bool fullscreen_ = false;
  bool fit_to_window_ = false;
  int scroll_x_ = 0;
  int scroll_y_ = 0;
  bool audit_clipboard_sent_ = false;
  bool audit_file_sent_ = false;
  int audit_file_out_count_ = 0;
  int audit_file_in_count_ = 0;
  int audit_file_out_entries_ = 0;
  int audit_file_in_entries_ = 0;
  std::vector<road_desk::media::MediaAuditFileItem> audit_file_items_;
  bool audit_opened_sent_ = false;
  std::string audit_session_id_;
  std::string audit_agent_id_;
  std::string audit_agent_name_;
  std::string audit_agent_endpoint_;
  // If non-null, fullscreen was entered from a docked child; Esc should re-attach here.
  HWND fullscreen_dock_parent_ = nullptr;
  RECT fullscreen_restore_rect_{};
  LONG_PTR fullscreen_restore_style_ = 0;
  ClosedFn on_closed_;

  HDC back_dc_ = nullptr;
  HBITMAP back_bmp_ = nullptr;
  HGDIOBJ back_old_ = nullptr;
  int back_w_ = 0;
  int back_h_ = 0;
  bool tracking_leave_ = false;
  int last_ptr_mask_ = -1;

  // Cached desktop (no cursor); refreshed when MediaClient framebuffer_epoch changes.
  std::vector<uint8_t> desk_bgra_;
  int desk_w_ = 0;
  int desk_h_ = 0;
  uint32_t desk_epoch_ = 0;
};

}  // namespace road_desk::viewer
