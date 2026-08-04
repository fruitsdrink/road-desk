#include "log.h"

#include "media_log.h"

namespace road_desk::agent {

bool init_log() {
  return media::media_log_open("host-agent.log");
}

void log_line(const char* message) {
  media::media_logf("host-agent", "%s", message ? message : "");
}

}  // namespace road_desk::agent
