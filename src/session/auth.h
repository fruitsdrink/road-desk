#pragma once

#include <string>

// Control-plane auth helpers. MVP: pre-shared passphrase only.

namespace road_desk::session {

bool authenticate_psk(const std::string& expected, const std::string& provided);

}  // namespace road_desk::session
