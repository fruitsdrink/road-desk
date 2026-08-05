#pragma once

// Counts active media sessions in-process (not a cross-process lock).
// Exclusive control is enforced separately in mux_host (A5a: one controller).

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
