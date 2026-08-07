#include "session_mutex.h"

#include <cstdio>

namespace {

int g_fails = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_fails;
  }
}

void test_initial_state() {
  using namespace road_desk::session;
  SessionMutex m;
  expect(!m.held(), "initial held false");
  expect(m.count() == 0, "initial count 0");
}

void test_single_acquire() {
  using namespace road_desk::session;
  SessionMutex m;
  expect(m.try_acquire(), "try_acquire success");
  expect(m.held(), "held after acquire");
  expect(m.count() == 1, "count 1 after acquire");
}

void test_multiple_acquire() {
  using namespace road_desk::session;
  SessionMutex m;
  for (int i = 1; i <= 8; ++i) {
    expect(m.try_acquire(), "try_acquire n");
    expect(m.count() == static_cast<unsigned>(i), "count after acquire n");
  }
  expect(m.held(), "held after 8 acquires");
}

void test_release_reduces_count() {
  using namespace road_desk::session;
  SessionMutex m;
  m.try_acquire();
  m.try_acquire();
  expect(m.count() == 2, "count 2");
  m.release();
  expect(m.count() == 1, "count 1 after release");
  expect(m.held(), "still held");
}

void test_release_to_zero() {
  using namespace road_desk::session;
  SessionMutex m;
  m.try_acquire();
  m.release();
  expect(m.count() == 0, "count 0 after release");
  expect(!m.held(), "not held at 0");
}

void test_release_underflow_guard() {
  using namespace road_desk::session;
  SessionMutex m;
  // Release without acquire must not wrap/underflow.
  m.release();
  m.release();
  expect(m.count() == 0, "count 0 no underflow");
  expect(!m.held(), "not held no underflow");
}

void test_try_acquire_always_succeeds() {
  using namespace road_desk::session;
  SessionMutex m;
  // try_acquire always returns true (no upper limit in this class;
  // capacity enforcement is in mux_host).
  for (int i = 0; i < 100; ++i) {
    expect(m.try_acquire(), "always true");
  }
  expect(m.count() == 100, "count 100");
}

void test_interleaved_acquire_release() {
  using namespace road_desk::session;
  SessionMutex m;
  m.try_acquire();
  m.try_acquire();
  m.release();
  m.try_acquire();
  expect(m.count() == 2, "interleaved count 2");
  m.release();
  m.release();
  expect(m.count() == 0, "interleaved count 0");
}

}  // namespace

int main() {
  test_initial_state();
  test_single_acquire();
  test_multiple_acquire();
  test_release_reduces_count();
  test_release_to_zero();
  test_release_underflow_guard();
  test_try_acquire_always_succeeds();
  test_interleaved_acquire_release();

  if (g_fails != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_fails);
    return 1;
  }
  std::printf("session_mutex_test: ok\n");
  return 0;
}
