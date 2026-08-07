#include "injection_pipe.h"
#include "session_monitor.h"

#include "log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")

// Forward declare injection functions — same ones mux_host.cpp calls.
// These are in src/media/mux_inject.cpp, linked via road_desk_media_host.
namespace road_desk::replace {
void inject_pointer(int button_mask, int x, int y);
void inject_vk(unsigned vk, bool down, bool extended = false);
void release_modifiers();
}

namespace road_desk::agent {
namespace {

// ── Service-side state ────────────────────────────────────────────

HANDLE g_pipe = INVALID_HANDLE_VALUE;
HANDLE g_helper_process = nullptr;
std::atomic<bool> g_helper_connected{false};
std::atomic<bool> g_pipe_running{false};

struct PendingInj {
  InjCmd cmd = InjCmd::kQuit;
  uint8_t button_mask = 0;
  int32_t x = 0, y = 0;
  uint16_t vk = 0;
  bool down = false;
  bool extended = false;
};
std::mutex g_queue_mu;
std::deque<PendingInj> g_pending;
constexpr size_t kMaxPending = 4;

void pipe_log(const char* msg) {
  char buf[192];
  std::snprintf(buf, sizeof(buf), "injection_pipe: %s", msg);
  log_line(buf);
}

// ── Implementation ────────────────────────────────────────────────

bool ipipe_server_start() {
  if (g_pipe_running.load()) return true;

  g_pipe = CreateNamedPipeW(
      kInjectionPipeName,
      PIPE_ACCESS_DUPLEX,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
      1, 512, 512, 0, nullptr);
  if (g_pipe == INVALID_HANDLE_VALUE) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "CreateNamedPipe failed err=%lu",
                  static_cast<unsigned long>(GetLastError()));
    pipe_log(buf);
    return false;
  }
  g_pipe_running.store(true);
  pipe_log("server started");
  return true;
}

void ipipe_server_stop() {
  g_pipe_running.store(false);
  if (g_helper_connected.load() && g_pipe != INVALID_HANDLE_VALUE) {
    uint8_t quit = static_cast<uint8_t>(InjCmd::kQuit);
    DWORD wrote = 0;
    WriteFile(g_pipe, &quit, 1, &wrote, nullptr);
  }
  g_helper_connected.store(false);
  if (g_pipe != INVALID_HANDLE_VALUE) {
    DisconnectNamedPipe(g_pipe);
    CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
  }
  if (g_helper_process) {
    TerminateProcess(g_helper_process, 0);
    CloseHandle(g_helper_process);
    g_helper_process = nullptr;
  }
  pipe_log("server stopped");
}

void ipipe_server_pump() {
  if (!g_pipe_running.load() || g_pipe == INVALID_HANDLE_VALUE) return;

  if (!g_helper_connected.load()) {
    if (ConnectNamedPipe(g_pipe, nullptr)) {
      g_helper_connected.store(true);
      pipe_log("helper connected");
    } else {
      DWORD err = GetLastError();
      if (err == ERROR_PIPE_CONNECTED) {
        g_helper_connected.store(true);
        pipe_log("helper connected (already)");
      }
    }
  }

  if (g_helper_connected.load()) {
    std::lock_guard<std::mutex> lock(g_queue_mu);
    while (!g_pending.empty()) {
      const PendingInj& inj = g_pending.front();
      uint8_t buf[10];
      DWORD to_write = 0;
      buf[0] = static_cast<uint8_t>(inj.cmd);
      switch (inj.cmd) {
        case InjCmd::kPointer:
          buf[1] = inj.button_mask;
          buf[2] = static_cast<uint8_t>(inj.x & 0xff);
          buf[3] = static_cast<uint8_t>((inj.x >> 8) & 0xff);
          buf[4] = static_cast<uint8_t>((inj.x >> 16) & 0xff);
          buf[5] = static_cast<uint8_t>((inj.x >> 24) & 0xff);
          buf[6] = static_cast<uint8_t>(inj.y & 0xff);
          buf[7] = static_cast<uint8_t>((inj.y >> 8) & 0xff);
          buf[8] = static_cast<uint8_t>((inj.y >> 16) & 0xff);
          buf[9] = static_cast<uint8_t>((inj.y >> 24) & 0xff);
          to_write = 10;
          break;
        case InjCmd::kKey:
          buf[1] = static_cast<uint8_t>(inj.vk & 0xff);
          buf[2] = static_cast<uint8_t>((inj.vk >> 8) & 0xff);
          buf[3] = inj.down ? 1 : 0;
          buf[4] = inj.extended ? 1 : 0;
          to_write = 5;
          break;
        case InjCmd::kReleaseMods:
        case InjCmd::kHeartbeat:
        case InjCmd::kQuit:
          to_write = 1;
          break;
      }
      DWORD wrote = 0;
      if (!WriteFile(g_pipe, buf, to_write, &wrote, nullptr) || wrote != to_write) {
        g_helper_connected.store(false);
        DisconnectNamedPipe(g_pipe);
        pipe_log("helper disconnected (write fail)");
        break;
      }
      g_pending.pop_front();
    }
  }

  if (!g_helper_connected.load()) {
    std::lock_guard<std::mutex> lock(g_queue_mu);
    while (!g_pending.empty()) {
      const PendingInj& inj = g_pending.front();
      switch (inj.cmd) {
        case InjCmd::kPointer:
          road_desk::replace::inject_pointer(inj.button_mask, inj.x, inj.y);
          break;
        case InjCmd::kKey:
          road_desk::replace::inject_vk(inj.vk, inj.down, inj.extended);
          break;
        case InjCmd::kReleaseMods:
          road_desk::replace::release_modifiers();
          break;
        default: break;
      }
      g_pending.pop_front();
    }
  }
}

static void enqueue(const PendingInj& inj) {
  std::lock_guard<std::mutex> lock(g_queue_mu);
  if (g_pending.size() >= kMaxPending) g_pending.pop_front();
  g_pending.push_back(inj);
}

bool ipipe_enqueue_pointer(uint8_t bm, int32_t x, int32_t y) {
  enqueue({InjCmd::kPointer, bm, x, y, 0, false, false});
  return true;
}
bool ipipe_enqueue_key(uint16_t vk, bool down, bool ext) {
  enqueue({InjCmd::kKey, 0, 0, 0, vk, down, ext});
  return true;
}
bool ipipe_enqueue_release_mods() {
  enqueue({InjCmd::kReleaseMods});
  return true;
}
bool ipipe_helper_connected() { return g_helper_connected.load(); }

bool ipipe_launch_helper() {
  if (g_helper_process) {
    DWORD exit = 0;
    if (GetExitCodeProcess(g_helper_process, &exit) && exit == STILL_ACTIVE) return true;
    CloseHandle(g_helper_process);
    g_helper_process = nullptr;
  }

  const DWORD csid = WTSGetActiveConsoleSessionId();
  if (csid == 0xFFFFFFFF) return false;

  HANDLE user_token = nullptr;
  if (!WTSQueryUserToken(csid, &user_token)) return false;

  char exe_path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
  char cmd_line[512];
  std::snprintf(cmd_line, sizeof(cmd_line), "\"%s\" --helper RoadDeskInjection", exe_path);

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  HANDLE dup_token = nullptr;
  if (!DuplicateTokenEx(user_token, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &dup_token)) {
    CloseHandle(user_token);
    return false;
  }
  CloseHandle(user_token);

  if (!CreateProcessAsUserA(dup_token, nullptr, cmd_line, nullptr, nullptr, FALSE,
                            0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(dup_token);
    return false;
  }
  CloseHandle(dup_token);
  CloseHandle(pi.hThread);
  g_helper_process = pi.hProcess;
  pipe_log("helper launched");
  return true;
}

bool ipipe_is_direct() { return !g_helper_connected.load(); }
bool ipipe_can_inject() { return true; }

int ipipe_run_helper(const char* pipe_name) {
  std::string full = "\\\\.\\pipe\\";
  full += pipe_name;

  HANDLE h = CreateFileA(full.c_str(), GENERIC_READ | GENERIC_WRITE,
                         0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return 1;

  DWORD mode = PIPE_READMODE_BYTE;
  SetNamedPipeHandleState(h, &mode, nullptr, nullptr);

  uint8_t buf[10];
  while (true) {
    DWORD got = 0;
    if (!ReadFile(h, buf, 1, &got, nullptr) || got != 1) break;
    InjCmd cmd = static_cast<InjCmd>(buf[0]);
    switch (cmd) {
      case InjCmd::kPointer:
        if (!ReadFile(h, buf + 1, 9, &got, nullptr) || got != 9) goto done;
        {
          uint8_t bm = buf[1];
          int32_t x = static_cast<int32_t>(buf[2] | (buf[3] << 8) | (buf[4] << 16) | (buf[5] << 24));
          int32_t y = static_cast<int32_t>(buf[6] | (buf[7] << 8) | (buf[8] << 16) | (buf[9] << 24));
          road_desk::replace::inject_pointer(bm, x, y);
        }
        break;
      case InjCmd::kKey:
        if (!ReadFile(h, buf + 1, 4, &got, nullptr) || got != 4) goto done;
        {
          uint16_t vk = static_cast<uint16_t>(buf[1] | (buf[2] << 8));
          road_desk::replace::inject_vk(vk, buf[3] != 0, buf[4] != 0);
        }
        break;
      case InjCmd::kReleaseMods:
        road_desk::replace::release_modifiers();
        break;
      case InjCmd::kQuit:
        CloseHandle(h);
        return 0;
      default: break;
    }
  }
done:
  CloseHandle(h);
  return 0;
}

}  // namespace

// Public wrappers exposed in the header, delegating to the anon-ns impls.
bool injection_pipe_server_start() { return ipipe_server_start(); }
void injection_pipe_server_stop() { ipipe_server_stop(); }
void injection_pipe_server_pump() { ipipe_server_pump(); }
bool injection_pipe_enqueue_pointer(uint8_t bm, int32_t x, int32_t y) { return ipipe_enqueue_pointer(bm, x, y); }
bool injection_pipe_enqueue_key(uint16_t vk, bool down, bool ext) { return ipipe_enqueue_key(vk, down, ext); }
bool injection_pipe_enqueue_release_mods() { return ipipe_enqueue_release_mods(); }
bool injection_pipe_helper_connected() { return ipipe_helper_connected(); }
bool launch_session_helper() { return ipipe_launch_helper(); }
bool injection_is_direct_mode() { return ipipe_is_direct(); }
bool injection_can_inject() { return ipipe_can_inject(); }
int run_session_helper(const char* p) { return ipipe_run_helper(p); }

}  // namespace road_desk::agent