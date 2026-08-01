#include "log.h"

#include "auth.h"
#include "media_plane.h"
#include "session_mutex.h"

#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>

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

void write_marker(const char* name, const char* text) {
  char path[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return;
  }
  char* slash = path;
  for (char* p = path; *p; ++p) {
    if (*p == '\\' || *p == '/') {
      slash = p + 1;
    }
  }
  if (static_cast<size_t>(path + MAX_PATH - slash) <= strlen(name)) {
    return;
  }
  memcpy(slash, name, strlen(name) + 1);
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") == 0 && f) {
    fputs(text, f);
    fclose(f);
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
  write_marker("host-agent.boot", "main\n");

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  write_marker("host-agent.boot", "signal_ok\n");

  if (!road_desk::agent::init_log()) {
    write_marker("host-agent.boot", "init_log_fail\n");
    std::fprintf(stderr, "failed to open host-agent.log\n");
    return 1;
  }
  write_marker("host-agent.boot", "init_log_ok\n");
  road_desk::agent::log_line("host-agent starting");

  road_desk::media::MediaPlaneConfig cfg;
  if (argc >= 2 && argv[1] && argv[1][0]) {
    cfg.listen_port = std::atoi(argv[1]);
  }
  if (argc >= 3 && argv[2] && argv[2][0]) {
    cfg.password = argv[2];
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
    write_marker("host-agent.boot", "psk_fail\n");
    return 1;
  }

  cfg.require_tls = !env_truthy("ROAD_DESK_ALLOW_PLAINTEXT");
  cfg.session_mutex = &g_session_mutex;

  {
    char line[256];
    std::snprintf(line, sizeof(line), "config port=%d tls=%d argc=%d", cfg.listen_port,
                  cfg.require_tls ? 1 : 0, argc);
    road_desk::agent::log_line(line);
  }

  write_marker("host-agent.boot", "before_media\n");
  road_desk::media::MediaPlane media;
  g_media = &media;
  write_marker("host-agent.boot", "media_ctor_ok\n");

  if (!media.listen(cfg)) {
    road_desk::agent::log_line("media plane listen failed (port busy / TLS cert?)");
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
  write_marker("host-agent.boot", "listening\n");

  media.serve();
  g_media = nullptr;
  road_desk::agent::log_line("host-agent stopped");
  write_marker("host-agent.boot", "stopped\n");
  return 0;
}
