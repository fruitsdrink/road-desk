#include "debug_http.h"
#include "gateway_client.h"
#include "gateway_config.h"
#include "audit_client.h"
#include "process_audit.h"
#include "log.h"
#include "service_host.h"
#include "session_monitor.h"
#include "injection_pipe.h"

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

void restore_cursors_best_effort() {
  SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
}

LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* info) {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  char* slash = std::strrchr(path, '\\');
  if (slash) {
    *slash = '\0';
  }
  char crash_path[MAX_PATH] = {};
  std::snprintf(crash_path, sizeof(crash_path), "%s\\host-agent.crash.log", path);

  const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
  const void* addr =
      info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;

  char mod_name[MAX_PATH] = "unknown";
  HMODULE mod = nullptr;
  if (addr &&
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(addr), &mod) &&
      mod) {
    GetModuleFileNameA(mod, mod_name, MAX_PATH);
  }

  HANDLE f = CreateFileA(crash_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    char buf[768];
    const int n = std::snprintf(
        buf, sizeof(buf),
        "host-agent unhandled exception code=0x%08lx addr=%p module=%s version=%s built=%s %s\r\n",
        static_cast<unsigned long>(code), addr, mod_name, ROAD_DESK_VERSION_STRING,
        ROAD_DESK_BUILD_DATE, ROAD_DESK_BUILD_TIME);
    if (n > 0) {
      DWORD wrote = 0;
      WriteFile(f, buf, static_cast<DWORD>(n), &wrote, nullptr);
    }
    CloseHandle(f);
  }

  restore_cursors_best_effort();
  return EXCEPTION_CONTINUE_SEARCH;
}

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

// ROAD_DESK_DEBUG_HTTP=0/false disables. Default enabled.
bool debug_http_enabled_impl() {
  char* env = nullptr;
  size_t len = 0;
  if (_dupenv_s(&env, &len, "ROAD_DESK_DEBUG_HTTP") != 0 || !env) {
    return true;
  }
  const bool off = (env[0] == '0' || env[0] == 'n' || env[0] == 'N' || env[0] == 'f' ||
                    env[0] == 'F');
  free(env);
  return !off;
}

}  // namespace

// Public alias for service_host.h's declaration.
bool road_desk::agent::debug_http_enabled() {
  return debug_http_enabled_impl();
}

// ── host_agent_serve: shared entry for foreground + service ──────

int road_desk::agent::host_agent_serve(int argc, char** argv, bool as_service) {
  HANDLE stop_ev = as_service ? road_desk::agent::service_stop_event() : NULL;

  // S2: Start session monitoring (HWND for foreground, polling for service).
  // In service mode, SCM forwards WTS events via SERVICE_CONTROL_SESSIONCHANGE.
  road_desk::agent::session_monitor_start(as_service);

  // S4: Start injection pipe server (service mode) or direct injection (foreground).
  if (as_service) {
    road_desk::agent::injection_pipe_server_start();
  }

  // Re-init log for service thread (foreground already did in main()).
  if (as_service) {
    if (!road_desk::agent::init_log()) return 1;
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char line[640];
    std::snprintf(line, sizeof(line),
                  "host-agent starting (mux, service) version=%s built=%s %s path=%s",
                  ROAD_DESK_VERSION_STRING, ROAD_DESK_BUILD_DATE, ROAD_DESK_BUILD_TIME, exe);
    road_desk::agent::log_line(line);
    road_desk::agent::log_line("running as Windows service (LocalSystem)");
  }

  // Service mode must still register with the gateway when agent.json exists, or the
  // Viewer can never discover the host (it would show offline / missing). Only the
  // interactive config *prompt* is skipped in a non-interactive service session.
  // --no-gateway remains an explicit opt-out.
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
    char* env = nullptr; size_t len = 0;
    if (_dupenv_s(&env, &len, "ROAD_DESK_PSK") == 0 && env) {
      cfg.password = env;
    }
    free(env);
  }
  if (cfg.password.empty()) cfg.password = "road-desk";

  if (!road_desk::session::authenticate_psk(cfg.password, cfg.password)) {
    road_desk::agent::log_line("PSK required — set ROAD_DESK_PSK or pass [password]; refusing listen");
    return 1;
  }

  cfg.require_tls = true;
  cfg.session_mutex = &g_session_mutex;

  road_desk::agent::GatewayConfig gcfg;
  bool gateway_ready = false;
  if (!as_service) {
    road_desk::agent::log_line("elevated (Administrator)");
  }
  if (!no_gateway) {
    if (!road_desk::agent::load_gateway_config(&gcfg) && !as_service) {
      road_desk::agent::log_line("gateway: no agent.json — prompting for config");
      road_desk::agent::prompt_gateway_config(&gcfg);
      if (!gcfg.gateway_url.empty()) {
        road_desk::agent::save_gateway_config(gcfg);
      }
    }
    if (!gcfg.gateway_url.empty() && !gcfg.agent_psk.empty() && !gcfg.agent_id.empty()) {
      gateway_ready = true;
      road_desk::agent::audit_set_agent(gcfg);
      cfg.audit_fn = &road_desk::agent::audit_on_media_event;
      cfg.audit_user = nullptr;
      road_desk::agent::log_line("audit: host upsert enabled");
    }
  } else {
    road_desk::agent::log_line("gateway: skipped (--no-gateway or service mode)");
  }

  {
    char line[256];
    std::snprintf(line, sizeof(line), "config port=%d tls=%d as_service=%d",
                  cfg.listen_port, cfg.require_tls ? 1 : 0, as_service ? 1 : 0);
    road_desk::agent::log_line(line);
  }

  road_desk::media::MediaPlane media;
  g_media = &media;

  if (!media.listen(cfg)) {
    road_desk::agent::log_line(
        "FATAL: media plane listen failed — usually port already in use");
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
  if (gateway_ready) {
    gateway.start(gcfg, media.bound_port(), ROAD_DESK_VERSION_STRING);
    road_desk::agent::log_line("gateway: client started");
  }

  road_desk::agent::DebugHttpServer debug_http;
  if (debug_http_enabled_impl()) {
    const int debug_port = media.bound_port() + 1;
    if (!debug_http.start(debug_port, cfg.password)) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "debug-http: failed to bind %d", debug_port);
      road_desk::agent::log_line(buf);
    }
  }

  media.serve_with_stop_event(stop_ev);

  road_desk::agent::process_audit_shutdown();
  debug_http.stop();
  gateway.stop();
  road_desk::agent::injection_pipe_server_stop();
  road_desk::agent::session_monitor_stop();
  g_media = nullptr;
  road_desk::agent::log_line(as_service ? "host-agent service stopped" : "host-agent stopped");
  road_desk::media::media_log_close();
  return 0;
}

// ── main (foreground only) ───────────────────────────────────────

int main(int argc, char** argv) {
  // S1: Service CLI dispatch happens before DPI/exception/log init.
  // Guard against nullptr when argv[1] is absent (double-click launch).
  if (argc > 1 && road_desk::agent::is_service_cli_arg(argv[1])) {
    return road_desk::agent::try_run_service_cli(argc, argv) ? 0 : 1;
  }

  SetProcessDPIAware();
  SetUnhandledExceptionFilter(on_unhandled_exception);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  if (!road_desk::agent::init_log()) {
    return 1;
  }
  {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char line[640];
    std::snprintf(line, sizeof(line),
                  "host-agent starting (mux) version=%s built=%s %s path=%s",
                  ROAD_DESK_VERSION_STRING, ROAD_DESK_BUILD_DATE, ROAD_DESK_BUILD_TIME, exe);
    road_desk::agent::log_line(line);
  }

  // Check elevation for foreground mode.
  {
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
      if (!env_truthy("ROAD_DESK_ALLOW_NONADMIN")) {
        road_desk::agent::log_line(
            "FATAL: not elevated — refusing start. host-agent requires Administrator.");
        return 1;
      }
      road_desk::agent::log_line("WARN: ROAD_DESK_ALLOW_NONADMIN=1 — starting without Administrator");
    } else {
      road_desk::agent::log_line("elevated (Administrator)");
    }
  }

  return road_desk::agent::host_agent_serve(argc, argv, /*as_service=*/false);
}
