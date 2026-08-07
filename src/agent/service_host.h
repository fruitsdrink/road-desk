#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

#include <string>

// Forward reference — session_monitor.h needs the kServiceName for logging ctx.
namespace road_desk::agent {

// Windows Service lifecycle (S1 pre-login remote control).
//
// Dual-mode entry points:
//   host-agent.exe                    → foreground (existing behavior)
//   host-agent.exe --install          → CreateService + start
//   host-agent.exe --uninstall        → stop + DeleteService
//   host-agent.exe --run-as-service   → ServiceMain (called by SCM)
//   host-agent.exe --helper <pipe>    → Session Helper entry (S4)
//
// Session change notifications (S2) arrive via SERVICE_CONTROL_SESSIONCHANGE
// in the control handler and are forwarded to session_monitor_on_change().

// Service name and display name (one per machine).
extern const wchar_t* kServiceName;  // L"RoadDeskAgentServ"
extern const wchar_t* kServiceDisplayName;

// --- CLI dispatch ---

// true if argv contains --install / --uninstall / --run-as-service / --helper.
bool is_service_cli_arg(const char* arg);

// Returns true after performing the requested CLI action.
// caller_main_argv0 = argv[0] (path to host-agent.exe for service registration).
bool try_run_service_cli(int argc, char** argv);

// --- Service entry point ---

// Called by try_run_service_cli when --run-as-service is passed.
// Blocks until service stop is requested. Returns 0 on clean shutdown.
int run_as_service(int argc, char** argv);

// --- Service helpers exposed for main.cpp ---

// Returns the service stop event handle (nullptr if not running as a service).
// The serve loop polls this to know when to shut down.
HANDLE service_stop_event();

// Shared serve entry point: used by both foreground main() and service ServiceMain.
int host_agent_serve(int argc, char** argv, bool as_service);

// ROAD_DESK_DEBUG_HTTP=0/false disables. Default enabled (used by both foreground and service).
bool debug_http_enabled();

}  // namespace road_desk::agent
