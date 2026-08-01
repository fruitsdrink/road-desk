#include "session_mutex.h"

namespace road_desk::session {

bool SessionMutex::try_acquire() {
  ++count_;
  return true;
}

void SessionMutex::release() {
  if (count_ > 0) {
    --count_;
  }
}

}  // namespace road_desk::session
