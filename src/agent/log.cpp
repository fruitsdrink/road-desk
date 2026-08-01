#include "log.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace road_desk::agent {
namespace {

FILE* g_file = nullptr;

bool exe_dir(char* out, size_t out_len) {
  char path[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH || out_len < 4) {
    return false;
  }
  char* slash = path;
  for (char* p = path; *p; ++p) {
    if (*p == '\\' || *p == '/') {
      slash = p;
    }
  }
  if (slash == path) {
    return false;
  }
  *slash = '\0';
  if (strlen(path) + 1 > out_len) {
    return false;
  }
  memcpy(out, path, strlen(path) + 1);
  return true;
}

}  // namespace

bool init_log() {
  if (g_file) {
    return true;
  }
  char dir[MAX_PATH] = {};
  if (!exe_dir(dir, sizeof(dir))) {
    return false;
  }
  char path[MAX_PATH] = {};
  if (sprintf_s(path, "%s\\host-agent.log", dir) <= 0) {
    return false;
  }
  if (fopen_s(&g_file, path, "a") != 0 || !g_file) {
    g_file = nullptr;
    return false;
  }
  setvbuf(g_file, nullptr, _IONBF, 0);
  return true;
}

void log_line(const std::string& message) {
  char ts[32] = {};
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_s(&tm, &t);
  std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
  if (g_file) {
    std::fprintf(g_file, "%s %s\n", ts, message.c_str());
    std::fflush(g_file);
  }
  std::fprintf(stdout, "%s %s\n", ts, message.c_str());
  std::fflush(stdout);
}

}  // namespace road_desk::agent
