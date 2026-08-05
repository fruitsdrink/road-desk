#include "audit_client.h"

#include "media_log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")

namespace road_desk::viewer {
namespace {

std::mutex g_dir_mu;
DirectoryConfig g_dir;
std::atomic<bool> g_dir_set{false};

std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        o += "\\\"";
        break;
      case '\\':
        o += "\\\\";
        break;
      case '\b':
        o += "\\b";
        break;
      case '\f':
        o += "\\f";
        break;
      case '\n':
        o += "\\n";
        break;
      case '\r':
        o += "\\r";
        break;
      case '\t':
        o += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          o += buf;
        } else {
          o.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return o;
}

bool parse_url(const std::string& url, std::wstring* host, INTERNET_PORT* port, bool* https,
               std::wstring* path_prefix) {
  std::wstring wurl(url.begin(), url.end());
  URL_COMPONENTSW uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t host_buf[256] = {};
  wchar_t path_buf[1024] = {};
  uc.lpszHostName = host_buf;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path_buf;
  uc.dwUrlPathLength = 1024;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    return false;
  }
  *host = host_buf;
  *port = uc.nPort;
  *https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
  *path_prefix = path_buf;
  while (!path_prefix->empty() && path_prefix->back() == L'/') {
    path_prefix->pop_back();
  }
  return !host->empty();
}

bool http_post_json_auth(const std::string& base_url, const std::string& path,
                         const std::string& bearer, const std::string& json_body, std::string* err) {
  std::wstring host;
  INTERNET_PORT port = 0;
  bool https = false;
  std::wstring prefix;
  if (!parse_url(base_url, &host, &port, &https, &prefix)) {
    *err = "bad gateway url";
    return false;
  }
  HINTERNET ses = WinHttpOpen(L"RoadDesk-Viewer-Audit/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) {
    *err = "WinHttpOpen failed";
    return false;
  }
  HINTERNET con = WinHttpConnect(ses, host.c_str(), port, 0);
  if (!con) {
    WinHttpCloseHandle(ses);
    *err = "connect failed";
    return false;
  }
  std::wstring wpath = prefix + std::wstring(path.begin(), path.end());
  DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET req = WinHttpOpenRequest(con, L"POST", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!req) {
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    *err = "open request failed";
    return false;
  }
  std::wstring headers = L"Content-Type: application/json\r\n";
  if (!bearer.empty()) {
    headers += L"Authorization: Bearer ";
    headers.append(bearer.begin(), bearer.end());
    headers += L"\r\n";
  }
  std::string payload = json_body;
  BOOL ok = WinHttpSendRequest(req, headers.c_str(), static_cast<DWORD>(-1),
                               payload.empty() ? WINHTTP_NO_REQUEST_DATA : payload.data(),
                               static_cast<DWORD>(payload.size()), static_cast<DWORD>(payload.size()),
                               0);
  if (ok) {
    ok = WinHttpReceiveResponse(req, nullptr);
  }
  DWORD status = 0;
  DWORD status_len = sizeof(status);
  if (ok) {
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len, WINHTTP_NO_HEADER_INDEX);
  }
  WinHttpCloseHandle(req);
  WinHttpCloseHandle(con);
  WinHttpCloseHandle(ses);
  if (!ok || status < 200 || status >= 300) {
    char line[64];
    std::snprintf(line, sizeof(line), "HTTP %lu", static_cast<unsigned long>(status));
    *err = line;
    return false;
  }
  return true;
}

std::string local_computer_name() {
  char buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
  DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
  if (GetComputerNameA(buf, &n)) {
    return buf;
  }
  return {};
}

std::string build_json(const AuditReport& r) {
  std::string j = "{";
  auto add_str = [&](const char* key, const std::string& v, bool* first) {
    if (v.empty()) {
      return;
    }
    if (!*first) {
      j += ',';
    }
    *first = false;
    j += '"';
    j += key;
    j += "\":\"";
    j += json_escape(v);
    j += '"';
  };
  auto add_bool = [&](const char* key, bool v, bool* first) {
    if (!*first) {
      j += ',';
    }
    *first = false;
    j += '"';
    j += key;
    j += "\":";
    j += v ? "true" : "false";
  };
  bool first = true;
  add_str("id", r.session_id, &first);
  add_str("source", "viewer", &first);
  add_str("phase", r.phase, &first);
  add_str("operatorName", r.operator_name, &first);
  add_str("viewerHost", r.viewer_host.empty() ? local_computer_name() : r.viewer_host, &first);
  add_str("agentId", r.agent_id, &first);
  add_str("agentName", r.agent_name, &first);
  add_str("agentEndpoint", r.agent_endpoint, &first);
  add_str("mode", r.mode, &first);
  add_str("result", r.result, &first);
  add_str("disconnectReason", r.disconnect_reason, &first);
  if (r.set_clipboard) {
    add_bool("usedClipboard", r.used_clipboard, &first);
  }
  if (r.set_file) {
    add_bool("usedFileTransfer", r.used_file_transfer, &first);
  }
  add_bool("partial", r.partial, &first);
  if (r.reconnect_count >= 0) {
    if (!first) {
      j += ',';
    }
    first = false;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "\"meta\":{\"reconnectCount\":%d}", r.reconnect_count);
    j += buf;
  }
  j += '}';
  return j;
}

}  // namespace

void audit_set_directory(const DirectoryConfig& cfg) {
  std::lock_guard<std::mutex> lock(g_dir_mu);
  g_dir = cfg;
  if (g_dir.directory_key.empty() && !g_dir.viewer_psk.empty()) {
    g_dir.directory_key = g_dir.viewer_psk;
  }
  g_dir_set.store(!g_dir.gateway_url.empty() && !g_dir.directory_key.empty());
}

DirectoryConfig audit_directory_copy() {
  std::lock_guard<std::mutex> lock(g_dir_mu);
  return g_dir;
}

bool audit_reporting_enabled() {
  return g_dir_set.load();
}

std::string audit_new_session_id() {
  GUID g{};
  if (CoCreateGuid(&g) != S_OK) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "00000000-0000-4000-8000-%012llx",
                  static_cast<unsigned long long>(GetTickCount64()));
    return buf;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                static_cast<unsigned long>(g.Data1), g.Data2, g.Data3, g.Data4[0], g.Data4[1],
                g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  // Lowercase for gateway UUID regex.
  for (char* p = buf; *p; ++p) {
    if (*p >= 'A' && *p <= 'F') {
      *p = static_cast<char>(*p - 'A' + 'a');
    }
  }
  return buf;
}

void audit_report_async(const AuditReport& report) {
  if (!audit_reporting_enabled() || report.session_id.empty()) {
    return;
  }
  DirectoryConfig dir;
  {
    std::lock_guard<std::mutex> lock(g_dir_mu);
    dir = g_dir;
  }
  if (dir.gateway_url.empty() || dir.directory_key.empty()) {
    return;
  }
  AuditReport copy = report;
  if (copy.operator_name.empty() && !dir.username.empty()) {
    copy.operator_name = dir.username;
  }
  if (copy.operator_name.empty() && !dir.viewer_psk.empty() &&
      dir.directory_key == dir.viewer_psk) {
    copy.operator_name = "viewer_psk";
  }
  std::string url = dir.gateway_url;
  std::string key = dir.directory_key;
  std::thread([url, key, copy]() {
    const std::string body = build_json(copy);
    std::string err;
    if (!http_post_json_auth(url, "/v1/audit/sessions/upsert", key, body, &err)) {
      road_desk::media::media_logf("viewer", "audit upsert fail phase=%s id=%s err=%s",
                                   copy.phase.c_str(), copy.session_id.c_str(), err.c_str());
    } else {
      road_desk::media::media_logf("viewer", "audit upsert ok phase=%s id=%s", copy.phase.c_str(),
                                   copy.session_id.c_str());
    }
  }).detach();
}

}  // namespace road_desk::viewer
