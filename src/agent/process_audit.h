#pragma once

#include <string>

namespace road_desk::agent {

// A5b/A5c: while a control session is active, poll process open/close and
// foreground window focus/title; batch-post to the gateway. No-op when audit off.
void process_audit_on_control_session(const std::string& session_id, const char* phase,
                                      const char* mode);

// Stop the monitor (agent shutdown). Safe if never started.
void process_audit_shutdown();

}  // namespace road_desk::agent
