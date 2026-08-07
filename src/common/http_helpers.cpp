#include "http_helpers.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")

#include <cstdio>
#include <cstring>

namespace road_desk::common {

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

bool parse_url(const std::string& url, std::wstring* host, unsigned short* port,
               bool* https, std::wstring* base_path) {
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
  *base_path = path_buf;
  while (!base_path->empty() && base_path->back() == L'/') {
    base_path->pop_back();
  }
  return !host->empty();
}

bool http_post_json(const std::string& base_url, const std::string& path,
                    const std::string& bearer, const std::string& json_body,
                    unsigned long timeout_ms, std::string* err) {
  std::wstring host;
  INTERNET_PORT port = 0;
  bool https = false;
  std::wstring prefix;
  if (!parse_url(base_url, &host, &port, &https, &prefix)) {
    *err = "bad gateway url";
    return false;
  }
  HINTERNET ses = WinHttpOpen(L"RoadDesk/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) {
    *err = "WinHttpOpen failed";
    return false;
  }
  if (timeout_ms > 0) {
    WinHttpSetTimeouts(ses, static_cast<int>(timeout_ms), static_cast<int>(timeout_ms),
                       static_cast<int>(timeout_ms), static_cast<int>(timeout_ms));
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
                               static_cast<DWORD>(payload.size()),
                               static_cast<DWORD>(payload.size()), 0);
  if (ok) {
    ok = WinHttpReceiveResponse(req, nullptr);
  }
  DWORD status = 0;
  DWORD status_len = sizeof(status);
  if (ok) {
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len,
                        WINHTTP_NO_HEADER_INDEX);
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

std::string new_session_id() {
  GUID g{};
  if (CoCreateGuid(&g) != S_OK) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "00000000-0000-4000-8000-%012llx",
                  static_cast<unsigned long long>(GetTickCount64()));
    return buf;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                static_cast<unsigned long>(g.Data1), g.Data2, g.Data3, g.Data4[0],
                g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5],
                g.Data4[6], g.Data4[7]);
  for (char* p = buf; *p; ++p) {
    if (*p >= 'A' && *p <= 'F') {
      *p = static_cast<char>(*p - 'A' + 'a');
    }
  }
  return buf;
}

std::string local_computer_name() {
  char buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
  DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
  if (GetComputerNameA(buf, &n)) {
    return buf;
  }
  return {};
}

}  // namespace road_desk::common
