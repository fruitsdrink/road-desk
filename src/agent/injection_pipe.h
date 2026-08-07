#pragma once

// S4: Session Helper — lightweight injection proxy running in the user session.
//
// Architecture:
//   Service (Session 0)  ──pipe──►  Session Helper (User Session)
//   * captures desktop              * receives injection commands
//   * does NOT inject directly      * calls SendInput locally
//                                   * ACKs back to service
//
// When the console is on the Logon or Locked desktop:
//   — No user session exists / token is not available
//   — Service calls SendInput directly (winlogon desktop is in Session 0)
//   — Session Helper is not needed

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <functional>

namespace road_desk::agent {

// Pipe name for IPC. Service creates the server end; Helper connects as client.
constexpr const wchar_t* kInjectionPipeName =
    L"\\\\.\\pipe\\RoadDeskInjection";

// Injection command types (tight binary format, no serialization overhead).
enum class InjCmd : uint8_t {
  kPointer = 1,    // | cmd(1) | button_mask(1) | x(4 LE) | y(4 LE) |  → 10 bytes
  kKey = 2,        // | cmd(1) | vk(1) | down(1) | extended(1) |        → 4 bytes
  kReleaseMods = 3,// | cmd(1) |                                         → 1 byte
  kHeartbeat = 4,  // | cmd(1) |                                         → 1 byte (keep-alive)
  kQuit = 5,       // | cmd(1) |                                         → 1 byte (shutdown helper)
};

// ── Service side (Session 0) ─────────────────────────────────────

// Create the named pipe server and start listening. Non-blocking.
bool injection_pipe_server_start();

// Stop the pipe server and signal the helper to quit.
void injection_pipe_server_stop();

// Queue an injection for the pump. Small backpressure queue.
bool injection_pipe_enqueue_pointer(uint8_t button_mask, int32_t x, int32_t y);
bool injection_pipe_enqueue_key(uint16_t vk, bool down, bool extended);
bool injection_pipe_enqueue_release_mods();

// Returns true if a Session Helper is currently connected.
bool injection_pipe_helper_connected();

// Try to launch the Session Helper into the console session.
bool launch_session_helper();

// ── Helper side (user session) ───────────────────────────────────

// Entry point for --helper <pipe_name>. Blocks until kQuit.
int run_session_helper(const char* pipe_name);

// ── Direct injection (logon/locked desktop fallback) ─────────────

bool injection_is_direct_mode();
bool injection_can_inject();

}  // namespace road_desk::agent
