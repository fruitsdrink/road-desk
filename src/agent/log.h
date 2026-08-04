#pragma once

#include <string>

namespace road_desk::agent {

// File log next to the executable (host-agent.log; shared with media plane).
bool init_log();
// Prefer const char* — avoids MSVCP std::string on constrained lab hosts.
void log_line(const char* message);
inline void log_line(const std::string& message) {
  log_line(message.c_str());
}

}  // namespace road_desk::agent
