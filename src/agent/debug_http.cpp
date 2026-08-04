#include "debug_http.h"

#include "log.h"

#include "media_log.h"
#include "product_version.h"
#include "session_recorder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace road_desk::agent {
namespace {

std::string exe_dir() {
  char buf[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  char* slash = std::strrchr(buf, '\\');
  if (slash) {
    *slash = '\0';
  }
  return buf;
}

bool read_file(const std::string& path, std::vector<uint8_t>* out) {
  // Prefer CreateFile with share flags — live host-agent.log is held open by the agent.
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return false;
  }
  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > 512ll * 1024 * 1024) {
    CloseHandle(h);
    return false;
  }
  out->resize(static_cast<size_t>(sz.QuadPart));
  if (sz.QuadPart > 0) {
    DWORD got = 0;
    if (!ReadFile(h, out->data(), static_cast<DWORD>(sz.QuadPart), &got, nullptr) ||
        got != static_cast<DWORD>(sz.QuadPart)) {
      CloseHandle(h);
      return false;
    }
  }
  CloseHandle(h);
  return true;
}

std::string url_decode(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      unsigned v = 0;
      if (sscanf_s(s.c_str() + i + 1, "%02x", &v) == 1) {
        o.push_back(static_cast<char>(v));
      }
      i += 2;
    } else if (s[i] == '+') {
      o.push_back(' ');
    } else {
      o.push_back(s[i]);
    }
  }
  return o;
}

bool path_under_debug(const std::string& name) {
  if (name.empty() || name.find("..") != std::string::npos) {
    return false;
  }
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
    return false;
  }
  return true;
}

void send_all(SOCKET s, const char* data, int len) {
  int off = 0;
  while (off < len) {
    const int n = send(s, data + off, len - off, 0);
    if (n <= 0) {
      return;
    }
    off += n;
  }
}

void send_response(SOCKET s, int code, const char* ctype, const uint8_t* body, size_t body_len) {
  char hdr[256];
  const char* reason = (code == 200) ? "OK" : (code == 401) ? "Unauthorized" : "Not Found";
  std::snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
                code, reason, ctype, body_len);
  send_all(s, hdr, static_cast<int>(std::strlen(hdr)));
  if (body && body_len) {
    send_all(s, reinterpret_cast<const char*>(body), static_cast<int>(body_len));
  }
}

bool auth_ok(const std::string& req, const std::string& psk) {
  if (psk.empty()) {
    return true;
  }
  const std::string bearer = "Authorization: Bearer " + psk;
  if (req.find(bearer) != std::string::npos) {
    return true;
  }
  const std::string q = "psk=" + psk;
  return req.find(q) != std::string::npos;
}

void handle_client(SOCKET cs, const std::string& psk) {
  char buf[4096] = {};
  const int n = recv(cs, buf, sizeof(buf) - 1, 0);
  if (n <= 0) {
    closesocket(cs);
    return;
  }
  const std::string req(buf, buf + n);
  if (!auth_ok(req, psk)) {
    const char* msg = "unauthorized";
    send_response(cs, 401, "text/plain", reinterpret_cast<const uint8_t*>(msg), std::strlen(msg));
    closesocket(cs);
    return;
  }

  std::string method;
  std::string path;
  {
    const size_t sp1 = req.find(' ');
    const size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : req.find(' ', sp1 + 1);
    if (sp1 != std::string::npos && sp2 != std::string::npos) {
      method = req.substr(0, sp1);
      path = req.substr(sp1 + 1, sp2 - sp1 - 1);
    }
  }
  std::string query;
  const size_t qpos = path.find('?');
  if (qpos != std::string::npos) {
    query = path.substr(qpos + 1);
    path = path.substr(0, qpos);
  }
  (void)query;

  if (method != "GET") {
    const char* msg = "method not allowed";
    send_response(cs, 404, "text/plain", reinterpret_cast<const uint8_t*>(msg), std::strlen(msg));
    closesocket(cs);
    return;
  }

  const std::string debug_dir = road_desk::media::session_recorder_debug_dir();

  if (path == "/v1/debug/health") {
    char body[256];
    std::snprintf(body, sizeof(body),
                  "{\"ok\":true,\"version\":\"%s\",\"built\":\"%s %s\",\"product\":\"host-agent\"}",
                  ROAD_DESK_VERSION_STRING, ROAD_DESK_BUILD_DATE, ROAD_DESK_BUILD_TIME);
    send_response(cs, 200, "application/json", reinterpret_cast<const uint8_t*>(body),
                  std::strlen(body));
    closesocket(cs);
    return;
  }

  if (path == "/v1/debug/version") {
    char body[256];
    std::snprintf(body, sizeof(body),
                  "{\"version\":\"%s\",\"built\":\"%s %s\"}", ROAD_DESK_VERSION_STRING,
                  ROAD_DESK_BUILD_DATE, ROAD_DESK_BUILD_TIME);
    send_response(cs, 200, "application/json", reinterpret_cast<const uint8_t*>(body),
                  std::strlen(body));
    closesocket(cs);
    return;
  }

  if (path == "/v1/debug/latest") {
    std::vector<uint8_t> body;
    const std::string p = debug_dir + "\\latest.json";
    if (!read_file(p, &body)) {
      const char* msg = "{}";
      send_response(cs, 200, "application/json", reinterpret_cast<const uint8_t*>(msg), 2);
    } else {
      send_response(cs, 200, "application/json", body.data(), body.size());
    }
    closesocket(cs);
    return;
  }

  if (path == "/v1/debug/list") {
    std::string json = "{\"files\":[";
    bool first = true;
    WIN32_FIND_DATAA fd{};
    const std::string pattern = debug_dir + "\\*";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          continue;
        }
        if (!first) {
          json += ",";
        }
        first = false;
        json += "\"";
        json += fd.cFileName;
        json += "\"";
      } while (FindNextFileA(h, &fd));
      FindClose(h);
    }
    // Live rotating log next to the exe (not under debug/).
    if (!first) {
      json += ",";
    }
    json += "\"host-agent.log\"";
    json += "]}";
    send_response(cs, 200, "application/json", reinterpret_cast<const uint8_t*>(json.data()),
                  json.size());
    closesocket(cs);
    return;
  }

  if (path.rfind("/v1/debug/file/", 0) == 0) {
    std::string name = url_decode(path.substr(std::strlen("/v1/debug/file/")));
    std::string full;
    const char* ctype = "application/octet-stream";
    if (name == "host-agent.log") {
      const char* lp = road_desk::media::media_log_path();
      if (lp && lp[0]) {
        full = lp;
      } else {
        // Fallback: <exe>/host-agent.log even if media log not opened yet.
        char mod[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, mod, MAX_PATH);
        char* slash = std::strrchr(mod, '\\');
        if (slash) {
          *slash = '\0';
        }
        full = std::string(mod) + "\\host-agent.log";
      }
      ctype = "text/plain; charset=utf-8";
    } else if (path_under_debug(name)) {
      full = debug_dir + "\\" + name;
      if (name.size() >= 4 && name.substr(name.size() - 4) == ".mp4") {
        ctype = "video/mp4";
      } else if (name.size() >= 4 && name.substr(name.size() - 4) == ".avi") {
        ctype = "video/x-msvideo";
      } else if (name.size() >= 5 && name.substr(name.size() - 5) == ".json") {
        ctype = "application/json";
      } else if (name.size() >= 4 && name.substr(name.size() - 4) == ".log") {
        ctype = "text/plain; charset=utf-8";
      }
    }
    std::vector<uint8_t> body;
    if (full.empty() || !read_file(full, &body)) {
      const char* msg = "not found";
      send_response(cs, 404, "text/plain", reinterpret_cast<const uint8_t*>(msg), std::strlen(msg));
    } else {
      send_response(cs, 200, ctype, body.data(), body.size());
    }
    closesocket(cs);
    return;
  }

  const char* msg = "not found";
  send_response(cs, 404, "text/plain", reinterpret_cast<const uint8_t*>(msg), std::strlen(msg));
  closesocket(cs);
}

}  // namespace

bool DebugHttpServer::start(int listen_port, const std::string& psk) {
  stop();
  if (listen_port <= 0 || listen_port > 65535) {
    return false;
  }
  SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ls == INVALID_SOCKET) {
    return false;
  }
  BOOL yes = TRUE;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<u_short>(listen_port));
  if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(ls, 8) != 0) {
    closesocket(ls);
    return false;
  }
  listen_ = static_cast<std::uintptr_t>(ls);
  port_ = listen_port;
  psk_ = psk;
  stop_ = false;
  thr_ = std::thread([this] { run(); });
  {
    char line[160];
    std::snprintf(line, sizeof(line),
                  "debug-http: listening on %d (GET /v1/debug/*) version=%s", port_,
                  ROAD_DESK_VERSION_STRING);
    log_line(line);
  }
  return true;
}

void DebugHttpServer::stop() {
  stop_ = true;
  if (listen_) {
    closesocket(static_cast<SOCKET>(listen_));
    listen_ = 0;
  }
  if (thr_.joinable()) {
    thr_.join();
  }
  port_ = 0;
}

void DebugHttpServer::run() {
  while (!stop_) {
    SOCKET ls = static_cast<SOCKET>(listen_);
    if (ls == INVALID_SOCKET || ls == 0) {
      break;
    }
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(ls, &rfds);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    const int sel = select(0, &rfds, nullptr, nullptr, &tv);
    if (sel <= 0) {
      continue;
    }
    SOCKET cs = accept(ls, nullptr, nullptr);
    if (cs == INVALID_SOCKET) {
      continue;
    }
    handle_client(cs, psk_);
  }
}

}  // namespace road_desk::agent
