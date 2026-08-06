#include "process_audit.h"

#include "audit_client.h"
#include "log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>

namespace road_desk::agent {
namespace {

constexpr DWORD kPollMs = 1000;
constexpr size_t kFlushBatch = 80;
constexpr DWORD kFirstFlushDelayMs = 2000;

struct ProcSnap {
  DWORD pid = 0;
  DWORD ppid = 0;
  std::string name;
  std::string path;
  std::string cmdline;
};

std::mutex g_mu;
std::string g_session_id;
std::atomic<bool> g_run{false};
HANDLE g_wake = nullptr;
std::thread g_thread;

using NtQueryInformationProcess_t = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG,
                                                       PULONG);

NtQueryInformationProcess_t nt_qip() {
  static NtQueryInformationProcess_t fn = reinterpret_cast<NtQueryInformationProcess_t>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
  return fn;
}

std::string wide_to_utf8(const wchar_t* w, int nchars = -1) {
  if (!w || nchars == 0 || (nchars < 0 && !w[0])) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, nchars, nullptr, 0, nullptr, nullptr);
  if (n <= 0) {
    return {};
  }
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, nchars, s.data(), n, nullptr, nullptr);
  if (nchars < 0 && !s.empty() && s.back() == '\0') {
    s.pop_back();
  }
  return s;
}

std::string iso_utc_now() {
  SYSTEMTIME st{};
  GetSystemTime(&st);
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", st.wYear, st.wMonth,
                st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  return buf;
}

std::string query_image_path(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) {
    return {};
  }
  wchar_t path[1024] = {};
  DWORD n = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
  std::string out;
  if (QueryFullProcessImageNameW(h, 0, path, &n) && n > 0) {
    out = wide_to_utf8(path);
  }
  CloseHandle(h);
  return out;
}

std::string query_cmdline(DWORD pid) {
  auto qip = nt_qip();
  if (!qip) {
    return {};
  }
  HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!h) {
    return {};
  }
  PROCESS_BASIC_INFORMATION pbi{};
  ULONG got = 0;
  const NTSTATUS st = qip(h, ProcessBasicInformation, &pbi, sizeof(pbi), &got);
  if (st < 0 || !pbi.PebBaseAddress) {
    CloseHandle(h);
    return {};
  }

#if defined(_WIN64)
  constexpr SIZE_T kPebParamsOff = 0x20;
  constexpr SIZE_T kCmdLineOff = 0x70;
#else
  constexpr SIZE_T kPebParamsOff = 0x10;
  constexpr SIZE_T kCmdLineOff = 0x40;
#endif

  PVOID params = nullptr;
  SIZE_T rd = 0;
  if (!ReadProcessMemory(h, reinterpret_cast<const BYTE*>(pbi.PebBaseAddress) + kPebParamsOff,
                         &params, sizeof(params), &rd) ||
      !params) {
    CloseHandle(h);
    return {};
  }

  UNICODE_STRING cmd{};
  if (!ReadProcessMemory(h, reinterpret_cast<const BYTE*>(params) + kCmdLineOff, &cmd, sizeof(cmd),
                         &rd) ||
      !cmd.Buffer || cmd.Length == 0 || cmd.Length > 32 * 1024) {
    CloseHandle(h);
    return {};
  }

  std::wstring w(cmd.Length / sizeof(wchar_t), L'\0');
  if (!ReadProcessMemory(h, cmd.Buffer, w.data(), cmd.Length, &rd)) {
    CloseHandle(h);
    return {};
  }
  CloseHandle(h);
  return wide_to_utf8(w.c_str(), static_cast<int>(w.size()));
}

bool skip_pid(DWORD pid) {
  return pid == 0 || pid == 4 || pid == GetCurrentProcessId();
}

std::unordered_map<DWORD, ProcSnap> snapshot_processes() {
  std::unordered_map<DWORD, ProcSnap> out;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return out;
  }
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      if (skip_pid(pe.th32ProcessID)) {
        continue;
      }
      ProcSnap s;
      s.pid = pe.th32ProcessID;
      s.ppid = pe.th32ParentProcessID;
      s.name = wide_to_utf8(pe.szExeFile);
      out.emplace(s.pid, std::move(s));
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return out;
}

void fill_details(ProcSnap* s) {
  if (!s) {
    return;
  }
  s->path = query_image_path(s->pid);
  s->cmdline = query_cmdline(s->pid);
}

AuditBehaviorEvent make_event(const std::string& session_id, const char* type, const ProcSnap& s) {
  AuditBehaviorEvent e;
  e.session_id = session_id;
  e.type = type;
  e.at_iso = iso_utc_now();

  std::string d = "{";
  bool first = true;
  auto add_str = [&](const char* k, const std::string& v) {
    if (v.empty()) {
      return;
    }
    if (!first) {
      d += ',';
    }
    first = false;
    d += '"';
    d += k;
    d += "\":\"";
    d += audit_json_escape(v);
    d += '"';
  };
  auto add_num = [&](const char* k, unsigned long long v) {
    if (!first) {
      d += ',';
    }
    first = false;
    d += '"';
    d += k;
    d += "\":";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu", v);
    d += buf;
  };
  add_str("name", s.name);
  add_str("path", s.path);
  add_num("pid", s.pid);
  if (s.ppid != 0) {
    add_num("ppid", s.ppid);
  }
  if (std::strcmp(type, "process_open") == 0) {
    add_str("cmdline", s.cmdline);
  }
  d += '}';
  e.detail_json = std::move(d);
  return e;
}

void stop_worker_join() {
  std::thread joiner;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_thread.joinable()) {
      g_session_id.clear();
      return;
    }
    g_run.store(false);
    if (g_wake) {
      SetEvent(g_wake);
    }
    joiner = std::move(g_thread);
    // Keep g_session_id until join so the worker's final flush still has the UUID.
  }
  if (joiner.joinable()) {
    joiner.join();
  }
  std::lock_guard<std::mutex> lock(g_mu);
  g_session_id.clear();
}

void worker_main() {
  std::unordered_map<DWORD, ProcSnap> known;
  bool seeded = false;
  std::vector<AuditBehaviorEvent> pending;
  pending.reserve(kFlushBatch);
  DWORD seed_tick = GetTickCount();
  size_t opens = 0;
  size_t closes = 0;

  auto flush_if_ready = [&](bool force) {
    if (pending.empty()) {
      return;
    }
    if (!force) {
      if (!seeded) {
        return;
      }
      if (GetTickCount() - seed_tick < kFirstFlushDelayMs && pending.size() < kFlushBatch) {
        return;
      }
    }
    std::string sid;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      sid = g_session_id;
    }
    if (sid.empty()) {
      pending.clear();
      return;
    }
    for (auto& e : pending) {
      e.session_id = sid;
    }
    audit_events_report_async(pending);
    pending.clear();
  };

  while (g_run.load()) {
    std::string sid;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      sid = g_session_id;
    }
    if (sid.empty()) {
      WaitForSingleObject(g_wake, kPollMs);
      continue;
    }

    auto cur = snapshot_processes();
    if (!seeded) {
      for (auto& kv : cur) {
        fill_details(&kv.second);
        known.emplace(kv.first, std::move(kv.second));
      }
      seeded = true;
      seed_tick = GetTickCount();
      char line[192];
      std::snprintf(line, sizeof(line), "process_audit seeded n=%u sid=%s",
                    static_cast<unsigned>(known.size()), sid.c_str());
      log_line(line);
    } else {
      for (auto& kv : cur) {
        if (known.find(kv.first) != known.end()) {
          continue;
        }
        fill_details(&kv.second);
        pending.push_back(make_event(sid, "process_open", kv.second));
        known.emplace(kv.first, kv.second);
        ++opens;
      }
      std::vector<DWORD> gone;
      for (const auto& kv : known) {
        if (cur.find(kv.first) == cur.end()) {
          gone.push_back(kv.first);
        }
      }
      for (DWORD pid : gone) {
        auto it = known.find(pid);
        if (it == known.end()) {
          continue;
        }
        pending.push_back(make_event(sid, "process_close", it->second));
        known.erase(it);
        ++closes;
      }
      flush_if_ready(pending.size() >= kFlushBatch);
    }

    WaitForSingleObject(g_wake, kPollMs);
    // Periodic flush of small batches after the FK delay.
    flush_if_ready(false);
  }

  flush_if_ready(true);
  char line[160];
  std::snprintf(line, sizeof(line), "process_audit stop opens=%u closes=%u",
                static_cast<unsigned>(opens), static_cast<unsigned>(closes));
  log_line(line);
}

void start_worker(const std::string& session_id) {
  stop_worker_join();
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_wake) {
      g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    g_session_id = session_id;
    g_run.store(true);
    g_thread = std::thread(worker_main);
    if (g_wake) {
      SetEvent(g_wake);
    }
  }
  char line[160];
  std::snprintf(line, sizeof(line), "process_audit start sid=%s", session_id.c_str());
  log_line(line);
}

}  // namespace

void process_audit_on_control_session(const std::string& session_id, const char* phase,
                                      const char* mode) {
  if (!phase) {
    return;
  }
  const bool is_control = mode && std::strcmp(mode, "control") == 0;
  const bool openish =
      std::strcmp(phase, "opened") == 0 || std::strcmp(phase, "flag") == 0;
  const bool closed = std::strcmp(phase, "closed") == 0;

  if (openish && is_control && !session_id.empty()) {
    if (!audit_reporting_enabled()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(g_mu);
      if (g_session_id == session_id && g_run.load() && g_thread.joinable()) {
        return;  // already monitoring this controller
      }
    }
    start_worker(session_id);
    return;
  }

  if (closed && !session_id.empty()) {
    bool ours = false;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      ours = (g_session_id == session_id);
    }
    if (ours) {
      stop_worker_join();
      log_line("process_audit stopped (control closed)");
    }
  }
}

void process_audit_shutdown() {
  stop_worker_join();
  if (g_wake) {
    CloseHandle(g_wake);
    g_wake = nullptr;
  }
}

}  // namespace road_desk::agent
