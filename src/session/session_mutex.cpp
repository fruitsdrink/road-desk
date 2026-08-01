#include "session_mutex.h"

namespace road_desk::session {

bool SessionMutex::try_acquire() {
  if (held_) {
    return false;
  }
  held_ = true;
  return true;
}

void SessionMutex::release() {
  held_ = false;
}

}  // namespace road_desk::session
