#include "log_util.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace road_desk::replace {
namespace {

FILE* g_fp = nullptr;
char g_path[MAX_PATH] = {};
CRITICAL_SECTION g_cs;
bool g_cs_ready = false;
bool g_mirror_stderr = true;

void ensure_cs() {
  if (!g_cs_ready) {
    InitializeCriticalSection(&g_cs);
    g_cs_ready = true;
  }
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

bool log_open(const char* filename) {
  ensure_cs();
  EnterCriticalSection(&g_cs);
  if (g_fp) {
    std::fclose(g_fp);
    g_fp = nullptr;
  }
  g_path[0] = '\0';
  if (!filename || !filename[0]) {
    LeaveCriticalSection(&g_cs);
    return false;
  }
  char dir[MAX_PATH] = {};
  exe_dir(dir, sizeof(dir));
  std::snprintf(g_path, sizeof(g_path), "%s\\%s", dir, filename);

  // "wb" = truncate each startup (overwrite previous run).
  if (fopen_s(&g_fp, g_path, "wb") != 0 || !g_fp) {
    g_fp = nullptr;
    std::fprintf(stderr, "[log] open failed path=%s (stderr only)\n", g_path);
    LeaveCriticalSection(&g_cs);
    return false;
  }
  char ts[64];
  format_ts(ts, sizeof(ts));
  std::fprintf(g_fp, "%s === log start (overwrite) path=%s pid=%lu ===\n", ts, g_path,
               static_cast<unsigned long>(GetCurrentProcessId()));
  std::fflush(g_fp);
  std::fprintf(stderr, "[log] writing %s (overwrite each start)\n", g_path);
  LeaveCriticalSection(&g_cs);
  return true;
}

void log_close() {
  if (!g_cs_ready) {
    return;
  }
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

void log_set_mirror_stderr(bool on) {
  ensure_cs();
  EnterCriticalSection(&g_cs);
  g_mirror_stderr = on;
  LeaveCriticalSection(&g_cs);
}

void logf(const char* tag, const char* fmt, ...) {
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
  if (g_mirror_stderr || !g_fp) {
    std::fprintf(stderr, "[%s] %s\n", t, line);
  }
  if (g_fp) {
    std::fprintf(g_fp, "%s [%s] %s\n", ts, t, line);
    std::fflush(g_fp);
  }
  LeaveCriticalSection(&g_cs);
}

const char* log_path() {
  return g_path;
}

}  // namespace road_desk::replace
