#include "log.h"

#include "media_log.h"

#include <cstdio>
#include <ctime>

namespace road_desk::agent {

bool init_log() {
  // One file for agent + media plane: host-agent.log next to the exe.
  return media::media_log_open("host-agent.log");
}

void log_line(const std::string& message) {
  media::media_logf("host-agent", "%s", message.c_str());
  // Also echo to stdout (media_log may have stderr mirror disabled in-session).
  char ts[32] = {};
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_s(&tm, &t);
  std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
  std::fprintf(stdout, "%s %s\n", ts, message.c_str());
  std::fflush(stdout);
}

}  // namespace road_desk::agent
