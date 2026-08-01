#include "connect_config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdlib>
#include <cstring>

namespace road_desk::viewer {
namespace {

// Same MVP baked cert as host (tls_mvp_cert_data.cpp).
constexpr char kMvpFingerprint[] =
    "39a0462cd62e85319ef4d768f88a12988bfe5f76f53c125d178b21fc7f1c4086";

std::string env_or_empty(const char* key) {
  char* env = nullptr;
  size_t len = 0;
  std::string out;
  if (_dupenv_s(&env, &len, key) == 0 && env) {
    out = env;
  }
  free(env);
  return out;
}

bool env_truthy(const char* key) {
  const std::string v = env_or_empty(key);
  return !v.empty() && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y');
}

}  // namespace

ConnectDefaults load_connect_defaults() {
  ConnectDefaults d;
  const std::string host = env_or_empty("ROAD_DESK_DEMO_HOST");
  const std::string port = env_or_empty("ROAD_DESK_DEMO_PORT");
  if (!host.empty() && !port.empty()) {
    d.host_port = host + ":" + port;
  } else if (!host.empty()) {
    d.host_port = host.find(':') != std::string::npos ? host : (host + ":38471");
  } else if (!port.empty()) {
    d.host_port = std::string("192.168.26.131:") + port;
  }

  {
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "ROAD_DESK_PSK") == 0 && env) {
      d.password = env;
    }
    free(env);
  }

  d.tls_fingerprint = env_or_empty("ROAD_DESK_TLS_FINGERPRINT");
  if (d.tls_fingerprint.empty()) {
    d.tls_fingerprint = kMvpFingerprint;
  }
  d.tls_insecure = env_truthy("ROAD_DESK_TLS_INSECURE");
  if (env_truthy("ROAD_DESK_ALLOW_PLAINTEXT")) {
    d.require_tls = false;
  }
  return d;
}

void apply_connect_defaults_to(road_desk::media::MediaClientConfig* cfg,
                               const ConnectDefaults& d) {
  if (!cfg) {
    return;
  }
  cfg->host_port = d.host_port;
  cfg->password = d.password;
  cfg->require_tls = d.require_tls;
  cfg->tls_insecure = d.tls_insecure;
  cfg->tls_fingerprint_sha256 = d.tls_fingerprint;
}

std::wstring connect_host_display(const ConnectDefaults& d) {
  std::string host = d.host_port;
  const size_t colon = host.rfind(':');
  // Avoid chopping IPv6; demo uses IPv4 host:port.
  if (colon != std::string::npos && host.find(':') == colon) {
    host = host.substr(0, colon);
  }
  if (host.empty()) {
    host = "192.168.26.131";
  }
  wchar_t wide[256] = {};
  MultiByteToWideChar(CP_ACP, 0, host.c_str(), -1, wide, 256);
  return wide;
}

int connect_port(const ConnectDefaults& d) {
  const size_t colon = d.host_port.rfind(':');
  if (colon == std::string::npos || d.host_port.find(':') != colon) {
    return 38471;
  }
  const int p = std::atoi(d.host_port.c_str() + colon + 1);
  return p > 0 ? p : 38471;
}

bool parse_direct_args(const char* narrow_cmd, ConnectDefaults* out) {
  if (!narrow_cmd || !out || !narrow_cmd[0]) {
    return false;
  }
  char buf[512] = {};
  strncpy_s(buf, narrow_cmd, _TRUNCATE);
  char* next = nullptr;
  char* tok = strtok_s(buf, " \t", &next);
  if (!tok) {
    return false;
  }
  out->host_port = tok;
  tok = strtok_s(nullptr, " \t", &next);
  if (tok) {
    out->password = tok;
  }
  tok = strtok_s(nullptr, " \t", &next);
  if (tok) {
    out->tls_fingerprint = tok;
  }
  return true;
}

}  // namespace road_desk::viewer
