#pragma once

#include <string>

namespace road_desk::agent {

// File log next to the executable (host-agent.log; shared with media plane).
// Sidecar serves it via GET /logs.
bool init_log();
void log_line(const std::string& message);

}  // namespace road_desk::agent
