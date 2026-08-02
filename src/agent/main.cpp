#include "gateway_client.h"
#include "gateway_config.h"
#include "log.h"

#include "auth.h"
#include "media_log.h"
#include "media_plane.h"
#include "product_version.h"
#include "session_mutex.h"

#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

road_desk::media::MediaPlane* g_media = nullptr;
road_desk::session::SessionMutex g_session_mutex;

void on_signal(int) {
  if (g_media) {
    g_media->request_stop();
  }
}

bool env_truthy(const char* name) {
  char* env = nullptr;
  size_t len = 0;
  if (_dupenv_s(&env, &len, name) != 0 || !env) {
    return false;
  }
  const bool on = (env[0] == '1' || env[0] == 'y' || env[0] == 'Y' || env[0] == 't' ||
                   env[0] == 'T');
  free(env);
  return on;
}

}  // namespace

int main(int argc, char** argv) {
  SetProcessDPIAware();

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  if (!road_desk::agent::init_log()) {
    std::fprintf(stderr, "failed to open host-agent.log\n");
    return 1;
  }
  {
    char line[128];
    std::snprintf(line, sizeof(line), "host-agent starting (mux) version=%s",
                  ROAD_DESK_VERSION_STRING);
    road_desk::agent::log_line(line);
  }

  {
    // Manifest requests requireAdministrator; still fail-closed if somehow not elevated
    // (e.g. stripped manifest). Mirror HKLM Attach.ToDesktop scrub needs admin.
    BOOL elevated = FALSE;
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
      TOKEN_ELEVATION elev{};
      DWORD got = 0;
      if (GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &got)) {
        elevated = elev.TokenIsElevated ? TRUE : FALSE;
      }
      CloseHandle(tok);
    }
    if (!elevated) {
      road_desk::agent::log_line(
          "FATAL: not elevated — refusing start. host-agent requires Administrator "
          "(Mirror Attach.ToDesktop scrub; non-admin + Device Manager freezes input). "
          "Re-run elevated, or install/run as an admin/LocalSystem service.");
      std::fprintf(stderr,
                   "host-agent requires Administrator (UAC). Refusing to start.\n");
      return 1;
    }
    road_desk::agent::log_line("elevated (Administrator)");
  }

  bool no_gateway = false;
  std::vector<char*> positional;
  for (int i = 1; i < argc; ++i) {
    if (argv[i] && std::strcmp(argv[i], "--no-gateway") == 0) {
      no_gateway = true;
    } else if (argv[i] && argv[i][0]) {
      positional.push_back(argv[i]);
    }
  }

  road_desk::media::MediaPlaneConfig cfg;
  if (positional.size() >= 1) {
    cfg.listen_port = std::atoi(positional[0]);
  }
  if (positional.size() >= 2) {
    cfg.password = positional[1];
  }
  {
    // If ROAD_DESK_PSK is set (even empty), it overrides argv — empty => fail-closed.
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "ROAD_DESK_PSK") == 0 && env) {
      cfg.password = env;
    }
    free(env);
  }

  // Fail-closed: empty PSK is rejected (authenticate_psk).
  if (!road_desk::session::authenticate_psk(cfg.password, cfg.password)) {
    road_desk::agent::log_line("PSK required — set ROAD_DESK_PSK or pass [password]; refusing listen");
    return 1;
  }

  // Mux media plane has no plaintext path (TLS-only).
  cfg.require_tls = true;
  if (env_truthy("ROAD_DESK_ALLOW_PLAINTEXT")) {
    road_desk::agent::log_line(
        "WARN: ROAD_DESK_ALLOW_PLAINTEXT ignored — mux requires TLS");
  }
  // Shared-control counter (not exclusive). Host allows up to 8 concurrent viewers.
  cfg.session_mutex = &g_session_mutex;

  {
    char line[256];
    std::snprintf(line, sizeof(line), "config port=%d tls=%d argc=%d", cfg.listen_port,
                  cfg.require_tls ? 1 : 0, argc);
    road_desk::agent::log_line(line);
  }

  road_desk::media::MediaPlane media;
  g_media = &media;

  if (!media.listen(cfg)) {
    road_desk::agent::log_line(
        "FATAL: media plane listen failed — usually port already in use "
        "(another host-agent / VNC on same port). See host-agent.log.");
    std::fprintf(stderr,
                 "host-agent: listen failed (port %d busy?). Check host-agent.log\n",
                 cfg.listen_port);
    g_media = nullptr;
    return 1;
  }

  {
    char line[512];
    const std::string fp = media.tls_fingerprint_sha256();
    if (!fp.empty()) {
      std::snprintf(line, sizeof(line), "media plane TLS listening on port %d fingerprint=%s",
                    media.bound_port(), fp.c_str());
    } else {
      std::snprintf(line, sizeof(line), "media plane listening on port %d (plaintext debug)",
                    media.bound_port());
    }
    road_desk::agent::log_line(line);
  }

  road_desk::agent::GatewayClient gateway;
  if (!no_gateway) {
    road_desk::agent::GatewayConfig gcfg;
    if (!road_desk::agent::load_gateway_config(&gcfg)) {
      road_desk::agent::log_line("gateway: no agent.json — prompting for config");
      if (!road_desk::agent::prompt_gateway_config(&gcfg)) {
        road_desk::agent::log_line(
            "gateway: config cancelled — media stays up; use --no-gateway to skip");
      } else if (!road_desk::agent::save_gateway_config(gcfg)) {
        road_desk::agent::log_line("gateway: save agent.json failed — continuing in-memory");
      }
    }
    if (!gcfg.gateway_url.empty() && !gcfg.agent_psk.empty() && !gcfg.agent_id.empty()) {
      gateway.start(gcfg, media.bound_port(), ROAD_DESK_VERSION_STRING);
      road_desk::agent::log_line("gateway: client started (register/heartbeat; failures retry)");
    }
  } else {
    road_desk::agent::log_line("gateway: skipped (--no-gateway)");
  }

  media.serve();
  gateway.stop();
  g_media = nullptr;
  road_desk::agent::log_line("host-agent stopped");
  road_desk::media::media_log_close();
  return 0;
}
