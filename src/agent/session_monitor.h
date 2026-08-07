#pragma once

// S2: Session change monitor and host desktop state tracking for pre-login remote control.
//
// Tracks the active console session's desktop state (console / logon / locked / none).
// Used by both foreground and service modes. In service mode, WTS session notifications
// are delivered via the SCM control handler and forwarded here. In foreground mode,
// an HWND-based notification is registered if a message-only window is available.
//
// Also provides the current desktop DC handle for capture (S3).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wtsapi32.h>

#include <cstdint>

namespace road_desk::agent {

// Host desktop state reported to the Viewer via kCtrlHostState (S5).
enum class HostDesktopState : uint8_t {
  kConsole = 0,  // user logged in, on the default desktop
  kLogon = 1,    // login / winlogon desktop (no user session)
  kLocked = 2,   // user session exists but locked (secure desktop)
  kNone = 3,     // initial / unknown / error
};

// Returns the current best-guess desktop state.
HostDesktopState session_desktop_state();

// Start monitoring. In service mode, just sets up initial state (notifications
// arrive via svc_ctrl_handler). In foreground mode, creates a message-only
// window and registers WTS session notifications on it.
//
// Must be called after the process window station is ready (not needed for
// background-side polling-only mode).
bool session_monitor_start(bool as_service);

// Stop monitoring (foreground only — unregisters notifications, destroys window).
void session_monitor_stop();

// Called from the service control handler or foreground WNDPROC when a session
// change event fires.
void session_monitor_on_change(DWORD event_type, DWORD session_id);

// Returns a display string for the current state (e.g. "console", "logon").
const char* session_desktop_state_name();

// ── Desktop DC for capture (S3) ──────────────────────────────────

// Open a DC for the current target desktop. Caller must NOT close the returned
// HDC — it is managed internally and refreshed on session change.
//
// Returns nullptr if the target desktop is unavailable (DXGI fallback needed).
HDC session_capture_dc();

// The width and height of the current capture desktop, or 0.
int session_capture_width();
int session_capture_height();

// Force-refresh the desktop DC on the next capture call (e.g. after session change).
void session_capture_invalidate();

}  // namespace road_desk::agent
