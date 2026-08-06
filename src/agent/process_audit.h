#pragma once

#include <string>

namespace road_desk::agent {

// A5b: while a control session is active, poll process open/close and batch-post
// process_open / process_close to the gateway. No-op when audit reporting is off.
void process_audit_on_control_session(const std::string& session_id, const char* phase,
                                      const char* mode);

// Stop the monitor (agent shutdown). Safe if never started.
void process_audit_shutdown();

}  // namespace road_desk::agent
