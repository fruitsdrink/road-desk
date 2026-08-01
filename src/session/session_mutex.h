#pragma once

// MVP: one remote session per host Agent process at a time.
// In-process only — not a cross-process / multi-instance lock.

namespace road_desk::session {

class SessionMutex {
 public:
  bool try_acquire();
  void release();
  bool held() const { return held_; }

 private:
  bool held_ = false;
};

}  // namespace road_desk::session
