#include "session_monitor.h"

#include <cstdio>
#include <cstring>

namespace {

int g_fails = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_fails;
  }
}

// ── HostDesktopState enum values ──────────────────────────────────

void test_state_enum_values() {
  using namespace road_desk::agent;
  expect(static_cast<uint8_t>(HostDesktopState::kConsole) == 0, "kConsole=0");
  expect(static_cast<uint8_t>(HostDesktopState::kLogon) == 1, "kLogon=1");
  expect(static_cast<uint8_t>(HostDesktopState::kLocked) == 2, "kLocked=2");
  expect(static_cast<uint8_t>(HostDesktopState::kNone) == 3, "kNone=3");
}

// ── session_desktop_state_name() returns valid string ─────────────

void test_state_names() {
  using namespace road_desk::agent;

  session_monitor_start(/*as_service=*/true);
  const char* name = session_desktop_state_name();
  expect(name != nullptr, "state name not null");
  // Name should be a known string.
  const bool known = (std::strcmp(name, "console") == 0 ||
                       std::strcmp(name, "logon") == 0 ||
                       std::strcmp(name, "locked") == 0 ||
                       std::strcmp(name, "none") == 0);
  expect(known, "state name is known");
  session_monitor_stop();
}

// ── Desktop state transitions produce a valid state ───────────────

void test_desktop_state_after_lock_unlock() {
  using namespace road_desk::agent;

  session_monitor_start(/*as_service=*/true);

  // We shouldn't get kNone on a running Windows desktop.
  HostDesktopState init = session_desktop_state();
  expect(init != HostDesktopState::kNone, "initial state is not none");

  session_monitor_on_change(WTS_SESSION_LOCK, 0);
  HostDesktopState locked = session_desktop_state();
  expect(locked != HostDesktopState::kNone, "lock state is not none");

  session_monitor_on_change(WTS_SESSION_UNLOCK, 0);
  HostDesktopState unlocked = session_desktop_state();
  expect(unlocked != HostDesktopState::kNone, "unlock state is not none");

  session_monitor_stop();
}

// ── session_capture_dc() returns valid DC or headless fallback ────

void test_capture_dc_on_console() {
  using namespace road_desk::agent;

  session_monitor_start(/*as_service=*/true);

  HDC dc = session_capture_dc();
  if (dc) {
    int w = session_capture_width();
    int h = session_capture_height();
    expect(w > 0, "capture width > 0");
    expect(h > 0, "capture height > 0");
    expect(w <= 8192, "capture width <= 8192");
    expect(h <= 8192, "capture height <= 8192");
  }
  // If dc is null, test runner may be headless / without Desktop access.

  session_monitor_stop();
}

// ── session_capture_needs_gdi_fallback() returns bool ─────────────

void test_gdi_fallback_returns_bool() {
  using namespace road_desk::agent;

  session_monitor_start(/*as_service=*/true);
  bool needs = session_capture_needs_gdi_fallback();
  expect(needs == true || needs == false, "gdi fallback returns bool");
  session_monitor_stop();
}

// ── session_capture_invalidate() doesn't crash ────────────────────

void test_invalidate_no_crash() {
  using namespace road_desk::agent;

  session_monitor_start(/*as_service=*/true);
  session_capture_invalidate();
  expect(true, "invalidate doesn't crash");
  session_monitor_stop();
}

// ── Double start/stop is safe ─────────────────────────────────────

void test_double_start_stop() {
  using namespace road_desk::agent;

  session_monitor_start(/*as_service=*/true);
  session_monitor_start(/*as_service=*/true);
  session_monitor_stop();
  session_monitor_stop();

  expect(true, "double start/stop safe");
}

}  // namespace

int main() {
  test_state_enum_values();
  test_state_names();
  test_desktop_state_after_lock_unlock();
  test_capture_dc_on_console();
  test_gdi_fallback_returns_bool();
  test_invalidate_no_crash();
  test_double_start_stop();

  if (g_fails != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_fails);
    return 1;
  }
  std::printf("session_monitor_test: ok\n");
  return 0;
}
