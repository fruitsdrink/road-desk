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

class SessionHost {
 public:
  using ClosedFn = std::function<void(SessionHost*)>;

  SessionHost();
  ~SessionHost();

  SessionHost(const SessionHost&) = delete;
  SessionHost& operator=(const SessionHost&) = delete;

  static bool register_class(HINSTANCE instance);

  // Create child (docked) or top-level (floating) host window and start MediaClient.
  bool open(HINSTANCE instance, HWND parent_or_null, const std::wstring& title,
            const ConnectDefaults& connect, ClosedFn on_closed);

  void close();
  void clear_closed_handler() { on_closed_ = nullptr; }
  bool connected() const;
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

  road_desk::media::MediaClient client_;
  HWND hwnd_ = nullptr;
  bool floating_ = false;
  bool started_ = false;
  std::wstring title_;
  int device_id_ = -1;
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
