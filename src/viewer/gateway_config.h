#pragma once

#include <string>

namespace road_desk::viewer {

// Gateway directory access: viewer.psk or JWT from POST /v1/viewer/login.
struct DirectoryConfig {
  std::string gateway_url;
  std::string directory_key;  // session only: Viewer-PSK or Bearer JWT (not persisted)
  std::string viewer_psk;     // durable imported Viewer PSK (persisted)
  std::string username;       // optional; set after account login
  std::string password;       // UI/prefill only when remember_password; cleared if not remembered
  bool remember_password = false;
};

std::string directory_config_path();

DirectoryConfig load_directory_config();
bool save_directory_config(const DirectoryConfig& cfg);

// Modal: gateway host/port + account login and/or Viewer PSK import.
// On success fills gateway_url + directory_key (and username when login used).
// Persists gateway_url / viewer_psk / username; password only when remember_password;
// JWT is session-only.
bool prompt_directory_config(DirectoryConfig* inout);

// POST /v1/viewer/login → token.
bool viewer_login(const std::string& gateway_url, const std::string& username,
                  const std::string& password, std::string* token, std::string* err);

}  // namespace road_desk::viewer
