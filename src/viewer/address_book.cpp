#include "address_book.h"

#include "demo_book.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace road_desk::viewer {
namespace {

std::vector<BookNode> g_nodes;
AddressBookSource g_source = AddressBookSource::kDemo;

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
  if (n > 1) {
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  }
  return w;
}

std::string env_str(const char* name) {
  char* env = nullptr;
  size_t len = 0;
  if (_dupenv_s(&env, &len, name) != 0 || !env) {
    return {};
  }
  std::string s(env);
  free(env);
  return s;
}

bool parse_url(const std::string& url, std::wstring* host, INTERNET_PORT* port, bool* https,
               std::wstring* path_prefix) {
  URL_COMPONENTSW uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t host_buf[256] = {};
  wchar_t path_buf[512] = {};
  uc.lpszHostName = host_buf;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path_buf;
  uc.dwUrlPathLength = 512;
  std::wstring wurl(url.begin(), url.end());
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    return false;
  }
  *host = host_buf;
  *port = uc.nPort;
  *https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
  *path_prefix = path_buf;
  // drop trailing slash
  while (!path_prefix->empty() && path_prefix->back() == L'/') {
    path_prefix->pop_back();
  }
  return !host->empty();
}

bool http_get(const std::string& base_url, const std::string& path, const std::string& key,
              std::string* body, std::string* err) {
  std::wstring host;
  INTERNET_PORT port = 0;
  bool https = false;
  std::wstring prefix;
  if (!parse_url(base_url, &host, &port, &https, &prefix)) {
    *err = "bad gateway url";
    return false;
  }
  HINTERNET ses = WinHttpOpen(L"RoadDesk-Viewer/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
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
  HINTERNET req = WinHttpOpenRequest(con, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!req) {
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    *err = "open request failed";
    return false;
  }
  std::wstring auth = L"Authorization: Bearer ";
  auth += std::wstring(key.begin(), key.end());
  BOOL ok = WinHttpSendRequest(req, auth.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0,
                               0, 0);
  if (ok) {
    ok = WinHttpReceiveResponse(req, nullptr);
  }
  DWORD status = 0;
  DWORD status_len = sizeof(status);
  if (ok) {
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len, WINHTTP_NO_HEADER_INDEX);
  }
  if (!ok || status < 200 || status >= 300) {
    char line[64];
    std::snprintf(line, sizeof(line), "HTTP %lu", static_cast<unsigned long>(status));
    *err = line;
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return false;
  }
  std::string out;
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) {
      break;
    }
    std::string chunk(avail, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(req, chunk.data(), avail, &read)) {
      break;
    }
    chunk.resize(read);
    out += chunk;
  }
  WinHttpCloseHandle(req);
  WinHttpCloseHandle(con);
  WinHttpCloseHandle(ses);
  *body = std::move(out);
  return true;
}

// Minimal JSON helpers for directory tree response.
std::string json_extract_string_after(const std::string& s, size_t from, const char* key, size_t* next) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = s.find(needle, from);
  if (p == std::string::npos) {
    return {};
  }
  p = s.find(':', p + needle.size());
  if (p == std::string::npos) {
    return {};
  }
  p = s.find('"', p + 1);
  if (p == std::string::npos) {
    return {};
  }
  size_t end = p + 1;
  while (end < s.size()) {
    if (s[end] == '\\' && end + 1 < s.size()) {
      end += 2;
      continue;
    }
    if (s[end] == '"') {
      break;
    }
    ++end;
  }
  if (next) {
    *next = end + 1;
  }
  return s.substr(p + 1, end - p - 1);
}

long long json_extract_number_after(const std::string& s, size_t from, const char* key, size_t* next) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = s.find(needle, from);
  if (p == std::string::npos) {
    return 0;
  }
  p = s.find(':', p + needle.size());
  if (p == std::string::npos) {
    return 0;
  }
  ++p;
  while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) {
    ++p;
  }
  long long v = 0;
  bool neg = false;
  if (p < s.size() && s[p] == '-') {
    neg = true;
    ++p;
  }
  while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
    v = v * 10 + (s[p] - '0');
    ++p;
  }
  if (next) {
    *next = p;
  }
  return neg ? -v : v;
}

bool json_extract_bool_after(const std::string& s, size_t from, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = s.find(needle, from);
  if (p == std::string::npos) {
    return false;
  }
  p = s.find(':', p + needle.size());
  if (p == std::string::npos) {
    return false;
  }
  return s.find("true", p) < s.find("false", p) && s.find("true", p) != std::string::npos &&
         s.find("true", p) < p + 12;
}

int next_id = 1;

void add_group_tree(const std::string& json, size_t start, size_t end, int parent_id) {
  // Find top-level objects in array [ {...}, {...} ]
  size_t i = start;
  while (i < end) {
    size_t obj = json.find('{', i);
    if (obj == std::string::npos || obj >= end) {
      break;
    }
    int depth = 0;
    size_t j = obj;
    for (; j < end; ++j) {
      if (json[j] == '{') {
        ++depth;
      } else if (json[j] == '}') {
        --depth;
        if (depth == 0) {
          break;
        }
      }
    }
    if (j >= end) {
      break;
    }
    const size_t obj_end = j + 1;
    const long long gid = json_extract_number_after(json, obj, "id", nullptr);
    const std::string name = json_extract_string_after(json, obj, "name", nullptr);
    BookNode g;
    g.id = next_id++;
    g.parent_id = parent_id;
    g.kind = BookNodeKind::kGroup;
    g.name = utf8_to_wide(name.empty() ? ("group-" + std::to_string(gid)) : name);
    // stash gateway group id in remark for debugging
    g.remark = L"gid=" + std::to_wstring(gid);
    const int local_gid = g.id;
    g_nodes.push_back(g);

    // children array
    size_t ch = json.find("\"children\"", obj);
    if (ch != std::string::npos && ch < obj_end) {
      size_t arr = json.find('[', ch);
      if (arr != std::string::npos && arr < obj_end) {
        int d = 0;
        size_t k = arr;
        for (; k < obj_end; ++k) {
          if (json[k] == '[') {
            ++d;
          } else if (json[k] == ']') {
            --d;
            if (d == 0) {
              break;
            }
          }
        }
        if (k < obj_end) {
          add_group_tree(json, arr + 1, k, local_gid);
        }
      }
    }

    // agents array
    size_t ag = json.find("\"agents\"", obj);
    if (ag != std::string::npos && ag < obj_end) {
      size_t arr = json.find('[', ag);
      if (arr != std::string::npos && arr < obj_end) {
        int d = 0;
        size_t k = arr;
        for (; k < obj_end; ++k) {
          if (json[k] == '[') {
            ++d;
          } else if (json[k] == ']') {
            --d;
            if (d == 0) {
              break;
            }
          }
        }
        size_t p = arr + 1;
        while (p < k) {
          size_t aobj = json.find('{', p);
          if (aobj == std::string::npos || aobj >= k) {
            break;
          }
          int ad = 0;
          size_t ae = aobj;
          for (; ae < k; ++ae) {
            if (json[ae] == '{') {
              ++ad;
            } else if (json[ae] == '}') {
              --ad;
              if (ad == 0) {
                break;
              }
            }
          }
          std::string display = json_extract_string_after(json, aobj, "displayName", nullptr);
          std::string hostname = json_extract_string_after(json, aobj, "hostname", nullptr);
          std::string preferred = json_extract_string_after(json, aobj, "preferredIpv4", nullptr);
          std::string version = json_extract_string_after(json, aobj, "version", nullptr);
          int media_port = static_cast<int>(json_extract_number_after(json, aobj, "mediaPort", nullptr));
          bool online = json_extract_bool_after(json, aobj, "online");
          if (display.empty()) {
            display = hostname;
          }
          BookNode dev;
          dev.id = next_id++;
          dev.parent_id = local_gid;
          dev.kind = BookNodeKind::kDevice;
          dev.name = utf8_to_wide(display);
          dev.host = preferred;
          dev.port = media_port > 0 ? media_port : 38471;
          dev.version = version;
          dev.online = online;
          dev.role = online ? L"在线" : L"离线";
          if (!preferred.empty()) {
            dev.remark = utf8_to_wide(preferred + ":" + std::to_string(dev.port));
          }
          g_nodes.push_back(dev);
          p = ae + 1;
        }
      }
    }

    i = obj_end;
  }
}

void load_from_demo() {
  g_nodes.clear();
  next_id = 1;
  for (const DemoNode& d : demo_book_nodes()) {
    BookNode n;
    n.id = d.id;
    n.parent_id = d.parent_id;
    n.kind = (d.kind == DemoNodeKind::kDevice) ? BookNodeKind::kDevice : BookNodeKind::kGroup;
    n.name = d.name;
    n.role = d.role;
    n.remark = d.remark;
    n.online = true;
    g_nodes.push_back(n);
    if (d.id + 1 > next_id) {
      next_id = d.id + 1;
    }
  }
  g_source = AddressBookSource::kDemo;
}

}  // namespace

DirectoryConfig load_directory_config() {
  DirectoryConfig c;
  c.gateway_url = env_str("ROAD_DESK_GATEWAY_URL");
  c.directory_key = env_str("ROAD_DESK_VIEWER_KEY");
  if (c.directory_key.empty()) {
    c.directory_key = env_str("ROAD_DESK_DIRECTORY_KEY");  // legacy
  }
  // Optional file: %ProgramData%\RoadDesk\viewer.json
  if (c.gateway_url.empty() || c.directory_key.empty()) {
    char prog[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, prog))) {
      std::string path = std::string(prog) + "\\RoadDesk\\viewer.json";
      FILE* f = nullptr;
      if (fopen_s(&f, path.c_str(), "rb") == 0 && f) {
        std::string json;
        char buf[512];
        while (size_t n = fread(buf, 1, sizeof(buf), f)) {
          json.append(buf, n);
        }
        fclose(f);
        auto get = [&](const char* key) {
          const std::string needle = std::string("\"") + key + "\"";
          size_t p = json.find(needle);
          if (p == std::string::npos) {
            return std::string();
          }
          p = json.find('"', json.find(':', p));
          size_t end = json.find('"', p + 1);
          if (p == std::string::npos || end == std::string::npos) {
            return std::string();
          }
          return json.substr(p + 1, end - p - 1);
        };
        if (c.gateway_url.empty()) {
          c.gateway_url = get("gatewayUrl");
        }
        if (c.directory_key.empty()) {
          c.directory_key = get("viewerKey");
          if (c.directory_key.empty()) {
            c.directory_key = get("directoryKey");  // legacy
          }
        }
      }
    }
  }
  return c;
}

bool address_book_load(AddressBookSource source, const DirectoryConfig& dir, std::string* err) {
  if (source == AddressBookSource::kDemo) {
    load_from_demo();
    return true;
  }
  if (dir.gateway_url.empty() || dir.directory_key.empty()) {
    if (err) {
      *err = "ROAD_DESK_GATEWAY_URL / ROAD_DESK_VIEWER_KEY required";
    }
    return false;
  }
  std::string body;
  std::string e;
  if (!http_get(dir.gateway_url, "/v1/directory/tree", dir.directory_key, &body, &e)) {
    if (err) {
      *err = e;
    }
    return false;
  }
  g_nodes.clear();
  next_id = 1;
  size_t arr = body.find('[');
  size_t end = body.rfind(']');
  if (arr == std::string::npos || end == std::string::npos || end <= arr) {
    if (err) {
      *err = "bad directory tree json";
    }
    return false;
  }
  add_group_tree(body, arr + 1, end, -1);
  g_source = AddressBookSource::kGateway;
  return true;
}

AddressBookSource address_book_source() {
  return g_source;
}

const std::vector<BookNode>& address_book_nodes() {
  return g_nodes;
}

std::vector<const BookNode*> address_book_children(int parent_id) {
  std::vector<const BookNode*> out;
  for (const auto& n : g_nodes) {
    if (n.parent_id == parent_id) {
      out.push_back(&n);
    }
  }
  return out;
}

std::vector<const BookNode*> address_book_devices_under(int group_id) {
  std::vector<const BookNode*> out;
  const BookNode* self = address_book_find(group_id);
  if (!self) {
    for (const auto& n : g_nodes) {
      if (n.kind == BookNodeKind::kDevice) {
        out.push_back(&n);
      }
    }
    return out;
  }
  if (self->kind == BookNodeKind::kDevice) {
    out.push_back(self);
    return out;
  }
  std::vector<int> stack{group_id};
  while (!stack.empty()) {
    int id = stack.back();
    stack.pop_back();
    for (const BookNode* c : address_book_children(id)) {
      if (c->kind == BookNodeKind::kDevice) {
        out.push_back(c);
      } else {
        stack.push_back(c->id);
      }
    }
  }
  return out;
}

const BookNode* address_book_find(int id) {
  for (const auto& n : g_nodes) {
    if (n.id == id) {
      return &n;
    }
  }
  return nullptr;
}

}  // namespace road_desk::viewer
