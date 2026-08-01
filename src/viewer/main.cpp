#include "auth.h"
#include "connect_config.h"
#include "console_window.h"
#include "media_log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <string>

namespace {

void viewer_boot(const char* step) {
  if (!road_desk::media::media_log_is_open()) {
    road_desk::media::media_log_open("viewer.log");
  }
  road_desk::media::media_logf("viewer", "boot %s", step);
}

bool cmd_looks_like_direct(const wchar_t* cmd_line) {
  if (!cmd_line || !cmd_line[0]) {
    return false;
  }
  // Direct mode: first token contains ':' (host:port).
  for (const wchar_t* p = cmd_line; *p && *p != L' ' && *p != L'\t'; ++p) {
    if (*p == L':') {
      return true;
    }
  }
  return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmd_line, int show_cmd) {
  viewer_boot("main");
  SetProcessDPIAware();

  if (AllocConsole()) {
    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
  }
  viewer_boot("console_ok");

  road_desk::viewer::ConnectDefaults defaults =
      road_desk::viewer::load_connect_defaults();

  const bool direct = cmd_looks_like_direct(cmd_line);
  if (direct) {
    char narrow[512] = {};
    WideCharToMultiByte(CP_ACP, 0, cmd_line, -1, narrow, sizeof(narrow), nullptr, nullptr);
    road_desk::viewer::parse_direct_args(narrow, &defaults);
    // Re-apply env overrides that should still win for PSK / TLS.
    {
      char* env = nullptr;
      size_t len = 0;
      if (_dupenv_s(&env, &len, "ROAD_DESK_PSK") == 0 && env) {
        defaults.password = env;
      }
      free(env);
    }
    {
      char* env = nullptr;
      size_t len = 0;
      if (_dupenv_s(&env, &len, "ROAD_DESK_TLS_FINGERPRINT") == 0 && env && env[0]) {
        defaults.tls_fingerprint = env;
      }
      free(env);
    }
    {
      char* env = nullptr;
      size_t len = 0;
      if (_dupenv_s(&env, &len, "ROAD_DESK_TLS_INSECURE") == 0 && env &&
          (env[0] == '1' || env[0] == 'y' || env[0] == 'Y')) {
        defaults.tls_insecure = true;
      }
      free(env);
    }
  }

  if (!road_desk::session::authenticate_psk(defaults.password, defaults.password)) {
    viewer_boot("psk_fail");
    std::fprintf(stderr, "PSK required — set ROAD_DESK_PSK or pass password; refusing start\n");
    return 1;
  }
  if (defaults.require_tls && !defaults.tls_insecure && defaults.tls_fingerprint.empty()) {
    viewer_boot("tls_fp_missing");
    std::fprintf(stderr,
                 "TLS fingerprint required (ROAD_DESK_TLS_FINGERPRINT), "
                 "or set ROAD_DESK_TLS_INSECURE=1 for debug\n");
    return 1;
  }
  viewer_boot(direct ? "direct_mode" : "console_mode");

  const int rc = direct ? road_desk::viewer::run_direct_session(instance, show_cmd, defaults)
                        : road_desk::viewer::run_console(instance, show_cmd, defaults);

  viewer_boot("stopped");
  road_desk::media::media_log_close();
  return rc;
}
