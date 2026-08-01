#include "auth.h"

namespace road_desk::session {

bool authenticate_psk(const std::string& expected, const std::string& provided) {
  if (expected.empty()) {
    return false;
  }
  // Constant-time compare (length mismatch still fails closed).
  const size_t n = expected.size();
  const size_t m = provided.size();
  volatile unsigned char diff = static_cast<unsigned char>(n ^ m);
  for (size_t i = 0; i < n; ++i) {
    const unsigned char p = (i < m) ? static_cast<unsigned char>(provided[i]) : 0;
    diff = static_cast<unsigned char>(diff | (static_cast<unsigned char>(expected[i]) ^ p));
  }
  return diff == 0;
}

}  // namespace road_desk::session
