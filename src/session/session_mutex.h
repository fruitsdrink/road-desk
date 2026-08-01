#pragma once

// Shared-control: counts active media sessions in-process (not exclusive).
// Not a cross-process / multi-instance lock.

namespace road_desk::session {

class SessionMutex {
 public:
  // Always succeeds; increments active viewer count.
  bool try_acquire();
  void release();
  bool held() const { return count_ > 0; }
  unsigned count() const { return count_; }

 private:
  unsigned count_ = 0;
};

}  // namespace road_desk::session
