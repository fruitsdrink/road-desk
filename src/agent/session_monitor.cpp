#include "session_monitor.h"

#include "log.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "user32.lib")

namespace road_desk::agent {
namespace {

std::mutex g_state_mu;
HostDesktopState g_state = HostDesktopState::kNone;
bool g_monitoring = false;

// Current desktop DC for capture. Owned by this module, reopened on state change.
HDC g_capture_dc = nullptr;
HDESK g_capture_desk = nullptr;
int g_capture_w = 0;
int g_capture_h = 0;

// Foreground-only: HWND for WTS session notifications.
HWND g_session_hwnd = nullptr;
constexpr UINT WM_RD_SESSION_CHANGE = WM_APP + 100;

// ── Helpers ──────────────────────────────────────────────────────

DWORD active_console_session_id() {
  return WTSGetActiveConsoleSessionId();
}

HostDesktopState query_desktop_state() {
  const DWORD csid = active_console_session_id();
  if (csid == 0xFFFFFFFF) {
    return HostDesktopState::kNone;
  }

  // Try to open the user (default) desktop first.
  HDESK hDefault = OpenDesktopW(L"Default", 0, FALSE, DESKTOP_READOBJECTS);
  if (hDefault) {
    CloseDesktop(hDefault);
    HANDLE user_token = nullptr;
    if (WTSQueryUserToken(csid, &user_token)) {
      CloseHandle(user_token);
      return HostDesktopState::kConsole;
    }
    return HostDesktopState::kLogon;
  }

  // Default desktop not accessible. Try Winlogon desktop.
  HDESK hWinlogon = OpenDesktopW(L"Winlogon", 0, FALSE, DESKTOP_READOBJECTS);
  if (hWinlogon) {
    CloseDesktop(hWinlogon);
    HANDLE user_token = nullptr;
    if (WTSQueryUserToken(csid, &user_token)) {
      CloseHandle(user_token);
      return HostDesktopState::kLocked;
    }
    return HostDesktopState::kLogon;
  }

  return HostDesktopState::kNone;
}

const char* state_name(HostDesktopState st) {
  switch (st) {
    case HostDesktopState::kConsole: return "console";
    case HostDesktopState::kLogon:   return "logon";
    case HostDesktopState::kLocked:  return "locked";
    case HostDesktopState::kNone:    return "none";
    default:                         return "unknown";
  }
}

void close_capture_resources() {
  if (g_capture_dc) {
    ReleaseDC(nullptr, g_capture_dc);
    g_capture_dc = nullptr;
  }
  if (g_capture_desk) {
    CloseDesktop(g_capture_desk);
    g_capture_desk = nullptr;
  }
  g_capture_w = 0;
  g_capture_h = 0;
}

void open_capture_dc_for_state(HostDesktopState st) {
  close_capture_resources();

  const wchar_t* desk_name = L"Default";
  if (st == HostDesktopState::kLocked || st == HostDesktopState::kLogon) {
    desk_name = L"Winlogon";
  }

  g_capture_desk = OpenDesktopW(desk_name, 0, FALSE,
                                DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS);
  if (!g_capture_desk) {
    char line[160];
    std::snprintf(line, sizeof(line),
                  "session_monitor: OpenDesktop(%ls) failed err=%lu (state=%s)",
                  desk_name, static_cast<unsigned long>(GetLastError()), state_name(st));
    log_line(line);
    return;
  }

  // GetDCEx + HWND=NULL doesn't work with HDESK. Use GetDC.
  g_capture_dc = GetDC(nullptr);
  if (!g_capture_dc) {
    char line[160];
    std::snprintf(line, sizeof(line),
                  "session_monitor: GetDC failed err=%lu state=%s",
                  static_cast<unsigned long>(GetLastError()), state_name(st));
    log_line(line);
    CloseDesktop(g_capture_desk);
    g_capture_desk = nullptr;
    return;
  }

  g_capture_w = GetDeviceCaps(g_capture_dc, HORZRES);
  g_capture_h = GetDeviceCaps(g_capture_dc, VERTRES);

  char line[192];
  std::snprintf(line, sizeof(line),
                "session_monitor: capture DC %dx%d desktop=%ls state=%s",
                g_capture_w, g_capture_h, desk_name, state_name(st));
  log_line(line);
}

// ── WNDPROC for foreground session notifications ─────────────────

LRESULT CALLBACK session_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_RD_SESSION_CHANGE || msg == WM_WTSSESSION_CHANGE) {
    session_monitor_on_change(static_cast<DWORD>(wparam),
                              static_cast<DWORD>(lparam));
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

HostDesktopState session_desktop_state() {
  std::lock_guard<std::mutex> lock(g_state_mu);
  return g_state;
}

bool session_monitor_start(bool as_service) {
  if (g_monitoring) return true;

  g_state = query_desktop_state();
  open_capture_dc_for_state(g_state);

  char line[160];
  std::snprintf(line, sizeof(line),
                "session_monitor: start as_service=%d initial_state=%s",
                as_service ? 1 : 0, state_name(g_state));
  log_line(line);

  if (!as_service) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = session_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RoadDeskSessionMonitor";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      log_line("session_monitor: RegisterClassEx failed");
      return false;
    }
    g_session_hwnd = CreateWindowExW(0, L"RoadDeskSessionMonitor", L"",
                                     WS_OVERLAPPED, 0, 0, 0, 0, HWND_MESSAGE,
                                     nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_session_hwnd) {
      log_line("session_monitor: CreateWindowEx(HWND_MESSAGE) failed");
      return false;
    }
    if (!WTSRegisterSessionNotification(g_session_hwnd, NOTIFY_FOR_ALL_SESSIONS)) {
      char buf[128];
      std::snprintf(buf, sizeof(buf),
                    "session_monitor: WTSRegisterSessionNotification failed err=%lu",
                    static_cast<unsigned long>(GetLastError()));
      log_line(buf);
      DestroyWindow(g_session_hwnd);
      g_session_hwnd = nullptr;
      return false;
    }
  }

  g_monitoring = true;
  return true;
}

void session_monitor_stop() {
  if (!g_monitoring) return;

  if (g_session_hwnd) {
    WTSUnRegisterSessionNotification(g_session_hwnd);
    DestroyWindow(g_session_hwnd);
    g_session_hwnd = nullptr;
  }

  close_capture_resources();
  g_monitoring = false;
}

void session_monitor_on_change(DWORD event_type, DWORD /*session_id*/) {
  const char* name = "?";
  switch (event_type) {
    case WTS_CONSOLE_CONNECT:    name = "console_connect"; break;
    case WTS_CONSOLE_DISCONNECT: name = "console_disconnect"; break;
    case WTS_REMOTE_CONNECT:     name = "remote_connect"; break;
    case WTS_REMOTE_DISCONNECT:  name = "remote_disconnect"; break;
    case WTS_SESSION_LOGON:      name = "session_logon"; break;
    case WTS_SESSION_LOGOFF:     name = "session_logoff"; break;
    case WTS_SESSION_LOCK:       name = "session_lock"; break;
    case WTS_SESSION_UNLOCK:     name = "session_unlock"; break;
    default: return;
  }

  char line[128];
  std::snprintf(line, sizeof(line), "session_monitor: event=%s", name);
  log_line(line);

  {
    std::lock_guard<std::mutex> lock(g_state_mu);
    g_state = query_desktop_state();
    open_capture_dc_for_state(g_state);
    std::snprintf(line, sizeof(line), "session_monitor: new state=%s", state_name(g_state));
    log_line(line);
  }
}

const char* session_desktop_state_name() {
  return state_name(g_state);
}

HDC session_capture_dc() {
  return g_capture_dc;
}

int session_capture_width() {
  return g_capture_w;
}

int session_capture_height() {
  return g_capture_h;
}

void session_capture_invalidate() {
  std::lock_guard<std::mutex> lock(g_state_mu);
  g_state = query_desktop_state();
  open_capture_dc_for_state(g_state);
}

bool session_capture_needs_gdi_fallback() {
  std::lock_guard<std::mutex> lock(g_state_mu);
  return g_state == HostDesktopState::kLogon || g_state == HostDesktopState::kLocked;
}

}  // namespace road_desk::agent
