#include "media_log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <io.h>
#include <share.h>

namespace road_desk::media {
namespace {

FILE* g_fp = nullptr;
char g_path[MAX_PATH] = {};
CRITICAL_SECTION g_cs;
// Pure Win32 init — avoid std::call_once (MSVCP140 AV on some lab hosts right after log start).
INIT_ONCE g_cs_once = INIT_ONCE_STATIC_INIT;
bool g_mirror_stderr = true;

BOOL CALLBACK init_cs_once(PINIT_ONCE, PVOID, PVOID*) {
  InitializeCriticalSection(&g_cs);
  return TRUE;
}

void ensure_cs() {
  InitOnceExecuteOnce(&g_cs_once, init_cs_once, nullptr, nullptr);
}

// Elevated/task/service launches often leave CRT stderr/stdout as a stale handle.
bool crt_stream_safe(FILE* f) {
  if (!f) {
    return false;
  }
  const int fd = _fileno(f);
  if (fd < 0) {
    return false;
  }
  const intptr_t os = _get_osfhandle(fd);
  if (os < 0) {
    return false;
  }
  const HANDLE h = reinterpret_cast<HANDLE>(os);
  if (!h || h == INVALID_HANDLE_VALUE) {
    return false;
  }
  SetLastError(0);
  const DWORD ty = GetFileType(h);
  if (ty == FILE_TYPE_UNKNOWN && GetLastError() != NO_ERROR) {
    return false;
  }
  return ty == FILE_TYPE_CHAR || ty == FILE_TYPE_PIPE || ty == FILE_TYPE_DISK;
}

void mirror_stderr_line(const char* tag, const char* line) {
  if (!crt_stream_safe(stderr)) {
    return;
  }
  std::fprintf(stderr, "[%s] %s\n", tag && tag[0] ? tag : "log", line ? line : "");
  std::fflush(stderr);
}

void exe_dir(char* out, size_t out_len) {
  if (!out || out_len == 0) {
    return;
  }
  out[0] = '\0';
  char buf[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    std::strncpy(out, ".", out_len - 1);
    out[out_len - 1] = '\0';
    return;
  }
  char* slash = std::strrchr(buf, '\\');
  if (!slash) {
    slash = std::strrchr(buf, '/');
  }
  if (slash) {
    *slash = '\0';
    std::strncpy(out, buf, out_len - 1);
  } else {
    std::strncpy(out, ".", out_len - 1);
  }
  out[out_len - 1] = '\0';
}

void format_ts(char* out, size_t out_len) {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  std::snprintf(out, out_len, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
                static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond),
                static_cast<unsigned>(st.wMilliseconds));
}

}  // namespace

bool media_log_open(const char* filename) {
  ensure_cs();
  EnterCriticalSection(&g_cs);
  if (!filename || !filename[0]) {
    LeaveCriticalSection(&g_cs);
    return false;
  }
  char dir[MAX_PATH] = {};
  exe_dir(dir, sizeof(dir));
  char want[MAX_PATH] = {};
  std::snprintf(want, sizeof(want), "%s\\%s", dir, filename);

  if (g_fp && g_path[0] && _stricmp(g_path, want) == 0) {
    LeaveCriticalSection(&g_cs);
    return true;
  }

  if (g_fp) {
    std::fclose(g_fp);
    g_fp = nullptr;
  }
  std::snprintf(g_path, sizeof(g_path), "%s", want);

  g_fp = _fsopen(g_path, "wb", _SH_DENYWR);
  if (!g_fp) {
    g_fp = nullptr;
    char fail[512];
    std::snprintf(fail, sizeof(fail), "open failed path=%s (file log unavailable)", g_path);
    mirror_stderr_line("log", fail);
    LeaveCriticalSection(&g_cs);
    return false;
  }
  char ts[64];
  format_ts(ts, sizeof(ts));
  std::fprintf(g_fp, "%s === log start (overwrite) path=%s pid=%lu ===\n", ts, g_path,
               static_cast<unsigned long>(GetCurrentProcessId()));
  std::fflush(g_fp);
  LeaveCriticalSection(&g_cs);
  return true;
}

bool media_log_is_open() {
  ensure_cs();
  EnterCriticalSection(&g_cs);
  const bool open = g_fp != nullptr;
  LeaveCriticalSection(&g_cs);
  return open;
}

void media_log_close() {
  ensure_cs();
  EnterCriticalSection(&g_cs);
  if (g_fp) {
    char ts[64];
    format_ts(ts, sizeof(ts));
    std::fprintf(g_fp, "%s === log end ===\n", ts);
    std::fflush(g_fp);
    std::fclose(g_fp);
    g_fp = nullptr;
  }
  LeaveCriticalSection(&g_cs);
}

void media_log_set_mirror_stderr(bool on) {
  ensure_cs();
  EnterCriticalSection(&g_cs);
  g_mirror_stderr = on;
  LeaveCriticalSection(&g_cs);
}

void media_logf(const char* tag, const char* fmt, ...) {
  ensure_cs();
  char ts[64];
  format_ts(ts, sizeof(ts));

  char line[2048];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);

  const char* t = tag && tag[0] ? tag : "log";
  EnterCriticalSection(&g_cs);
  if ((g_mirror_stderr || !g_fp) && crt_stream_safe(stderr)) {
    std::fprintf(stderr, "[%s] %s\n", t, line);
    std::fflush(stderr);
  }
  if (g_fp) {
    std::fprintf(g_fp, "%s [%s] %s\n", ts, t, line);
    std::fflush(g_fp);
  }
  LeaveCriticalSection(&g_cs);
}

const char* media_log_path() {
  return g_path;
}

}  // namespace road_desk::media
