#include "address_book.h"

#include "demo_book.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

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

// Extract a JSON object or array value after key (balanced braces/brackets).
std::string json_extract_raw_after(const std::string& s, size_t from, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = s.find(needle, from);
  if (p == std::string::npos) {
    return {};
  }
  p = s.find(':', p + needle.size());
  if (p == std::string::npos) {
    return {};
  }
  ++p;
  while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) {
    ++p;
  }
  if (p >= s.size()) {
    return {};
  }
  if (s[p] == '"' ) {
    return json_extract_string_after(s, from, key, nullptr);
  }
  if (s[p] != '{' && s[p] != '[') {
    // number/bool/null — take until comma/brace
    size_t end = p;
    while (end < s.size() && s[end] != ',' && s[end] != '}' && s[end] != ']') {
      ++end;
    }
    return s.substr(p, end - p);
  }
  const char open = s[p];
  const char close = (open == '{') ? '}' : ']';
  int depth = 0;
  bool in_str = false;
  for (size_t i = p; i < s.size(); ++i) {
    const char c = s[i];
    if (in_str) {
      if (c == '\\' && i + 1 < s.size()) {
        ++i;
        continue;
      }
      if (c == '"') {
        in_str = false;
      }
      continue;
    }
    if (c == '"') {
      in_str = true;
      continue;
    }
    if (c == open) {
      ++depth;
    } else if (c == close) {
      --depth;
      if (depth == 0) {
        return s.substr(p, i - p + 1);
      }
    }
  }
  return {};
}

std::string join_json_string_array(const std::string& arr_json) {
  if (arr_json.size() < 2 || arr_json.front() != '[') {
    return {};
  }
  std::string out;
  size_t i = 1;
  while (i < arr_json.size()) {
    while (i < arr_json.size() && (arr_json[i] == ' ' || arr_json[i] == ',' || arr_json[i] == '\n')) {
      ++i;
    }
    if (i >= arr_json.size() || arr_json[i] == ']') {
      break;
    }
    if (arr_json[i] != '"') {
      break;
    }
    ++i;
    std::string item;
    while (i < arr_json.size()) {
      if (arr_json[i] == '\\' && i + 1 < arr_json.size()) {
        item.push_back(arr_json[i + 1]);
        i += 2;
        continue;
      }
      if (arr_json[i] == '"') {
        ++i;
        break;
      }
      item.push_back(arr_json[i++]);
    }
    if (!out.empty()) {
      out += ", ";
    }
    out += item;
  }
  return out;
}

std::string json_obj_string(const std::string& obj, const char* key) {
  return json_extract_string_after(obj, 0, key, nullptr);
}

long long json_obj_number(const std::string& obj, const char* key) {
  return json_extract_number_after(obj, 0, key, nullptr);
}

std::wstring format_uptime(long long sec) {
  if (sec <= 0) {
    return {};
  }
  const long long days = sec / 86400;
  const long long hours = (sec % 86400) / 3600;
  const long long mins = (sec % 3600) / 60;
  wchar_t buf[80];
  if (days > 0) {
    _snwprintf_s(buf, _TRUNCATE, L"%lld 天 %lld 小时 %lld 分", days, hours, mins);
  } else if (hours > 0) {
    _snwprintf_s(buf, _TRUNCATE, L"%lld 小时 %lld 分", hours, mins);
  } else {
    _snwprintf_s(buf, _TRUNCATE, L"%lld 分", mins > 0 ? mins : 1LL);
  }
  return buf;
}

std::string json_obj_num_str(const std::string& obj, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t q = obj.find(needle);
  if (q == std::string::npos) {
    return {};
  }
  q = obj.find(':', q + needle.size());
  if (q == std::string::npos) {
    return {};
  }
  ++q;
  while (q < obj.size() && (obj[q] == ' ' || obj[q] == '\t')) {
    ++q;
  }
  size_t e = q;
  if (e < obj.size() && obj[e] == '-') {
    ++e;
  }
  while (e < obj.size() && (obj[e] == '.' || (obj[e] >= '0' && obj[e] <= '9'))) {
    ++e;
  }
  if (e <= q) {
    return {};
  }
  return obj.substr(q, e - q);
}

bool json_obj_has_key(const std::string& obj, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  return obj.find(needle) != std::string::npos;
}

std::wstring format_inventory_section(const std::string& inv) {
  if (inv.empty() || inv == "{}") {
    return L"（尚无主机清单：确认 Host Agent ≥0.4.17 已心跳后，按 F5 刷新目录）\r\n";
  }
  std::wstring out;
  const std::string os = json_obj_string(inv, "os");
  const std::string osv = json_obj_string(inv, "osVersion");
  const std::string arch = json_obj_string(inv, "arch");
  const std::string cn = json_obj_string(inv, "computerName");
  const std::string cpu = json_obj_string(inv, "cpu");
  const long long mem = json_obj_number(inv, "memoryTotalMb");
  const long long cpu_count = json_obj_number(inv, "cpuCount");
  auto line = [&](const wchar_t* k, const std::string& v) {
    if (v.empty()) {
      return;
    }
    out += k;
    out += utf8_to_wide(v);
    out += L"\r\n";
  };
  auto line_w = [&](const wchar_t* k, const std::wstring& v) {
    if (v.empty()) {
      return;
    }
    out += k;
    out += v;
    out += L"\r\n";
  };
  line(L"操作系统：", os);
  line(L"系统版本：", osv);
  line(L"体系结构：", arch);
  line(L"计算机名：", cn);
  line(L"处理器：", cpu);
  if (cpu_count > 0) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld 逻辑核心", cpu_count);
    line(L"CPU 核心：", buf);
  }
  if (mem > 0) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lld MB", mem);
    line(L"物理内存：", buf);
  }

  // Live performance from latest heartbeat sample.
  const std::string cpu_usage = json_obj_num_str(inv, "cpuUsagePercent");
  const long long mem_load = json_obj_number(inv, "memoryLoadPercent");
  const long long mem_used = json_obj_number(inv, "memoryUsedMb");
  const long long mem_avail = json_obj_number(inv, "memoryAvailMb");
  const long long uptime = json_obj_number(inv, "uptimeSec");
  const bool have_perf = !cpu_usage.empty() || json_obj_has_key(inv, "memoryLoadPercent") ||
                         json_obj_has_key(inv, "uptimeSec");
  if (have_perf) {
    out += L"\r\n系统性能（最近心跳）\r\n";
    if (!cpu_usage.empty()) {
      line(L"CPU 占用：", cpu_usage + "%");
    }
    if (json_obj_has_key(inv, "memoryLoadPercent")) {
      char buf[96];
      if (mem_used > 0 || mem_avail > 0) {
        std::snprintf(buf, sizeof(buf), "%lld%%（已用 %lld MB / 可用 %lld MB）", mem_load, mem_used,
                      mem_avail);
      } else {
        std::snprintf(buf, sizeof(buf), "%lld%%", mem_load);
      }
      line(L"内存占用：", buf);
    }
    line_w(L"开机时长：", format_uptime(uptime));
  }

  const std::string disks = json_extract_raw_after(inv, 0, "disks");
  if (disks.size() > 2) {
    out += L"\r\n磁盘分区：\r\n";
    // Walk objects inside disks array roughly.
    size_t p = 0;
    while ((p = disks.find('{', p)) != std::string::npos) {
      size_t end = disks.find('}', p);
      if (end == std::string::npos) {
        break;
      }
      const std::string one = disks.substr(p, end - p + 1);
      const std::string letter = json_obj_string(one, "letter");
      const std::string label = json_obj_string(one, "label");
      const std::string fs = json_obj_string(one, "fs");
      char total_buf[32] = {};
      char free_buf[32] = {};
      auto extract_num_str = [&](const char* key, char* dest, size_t dest_n) {
        const std::string s = json_obj_num_str(one, key);
        if (s.empty() || s.size() >= dest_n) {
          dest[0] = 0;
          return;
        }
        std::memcpy(dest, s.data(), s.size());
        dest[s.size()] = 0;
      };
      extract_num_str("totalGb", total_buf, sizeof(total_buf));
      extract_num_str("freeGb", free_buf, sizeof(free_buf));
      out += L"  ";
      out += utf8_to_wide(letter);
      if (!label.empty()) {
        out += L" 「";
        out += utf8_to_wide(label);
        out += L"」";
      }
      if (!fs.empty()) {
        out += L" ";
        out += utf8_to_wide(fs);
      }
      if (total_buf[0]) {
        out += L" 共 ";
        out += utf8_to_wide(total_buf);
        out += L" GB";
      }
      if (free_buf[0]) {
        out += L" / 可用 ";
        out += utf8_to_wide(free_buf);
        out += L" GB";
      }
      // Used % when both totals parse as floats.
      if (total_buf[0] && free_buf[0]) {
        const double total_gb = std::atof(total_buf);
        const double free_gb = std::atof(free_buf);
        if (total_gb > 0.05) {
          const int used_pct =
              static_cast<int>((total_gb - free_gb) * 100.0 / total_gb + 0.5);
          wchar_t pct_buf[32];
          _snwprintf_s(pct_buf, _TRUNCATE, L"（已用 %d%%）", used_pct < 0 ? 0 : (used_pct > 100 ? 100 : used_pct));
          out += pct_buf;
        }
      }
      out += L"\r\n";
      p = end + 1;
    }
  }
  return out;
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
          std::string agent_id = json_extract_string_after(json, aobj, "agentId", nullptr);
          std::string last_seen = json_extract_string_after(json, aobj, "lastSeenAt", nullptr);
          std::string ipv4s_raw = json_extract_raw_after(json, aobj, "ipv4s");
          std::string tags_raw = json_extract_raw_after(json, aobj, "tagNames");
          std::string inventory = json_extract_raw_after(json, aobj, "inventory");
          std::string computer_role = json_extract_string_after(json, aobj, "computerRole", nullptr);
          std::string install_location =
              json_extract_string_after(json, aobj, "installLocation", nullptr);
          std::string lane_number = json_extract_string_after(json, aobj, "laneNumber", nullptr);
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
          dev.agent_id = agent_id;
          dev.hostname = hostname;
          dev.ipv4s = join_json_string_array(ipv4s_raw);
          dev.last_seen = last_seen;
          dev.tags = join_json_string_array(tags_raw);
          dev.inventory_json = inventory;
          dev.computer_role = utf8_to_wide(computer_role);
          dev.install_location = utf8_to_wide(install_location);
          dev.lane_number = utf8_to_wide(lane_number);
          dev.online = online;
          // role column is used as online status text in the console list.
          dev.role = online ? L"在线" : L"离线";
          // Do not stuff host:port into remark — list has IP / 端口 columns.
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

std::wstring address_book_format_detail(const BookNode* device) {
  if (!device || device->kind != BookNodeKind::kDevice) {
    return L"选择左侧列表中的被控端以查看详细信息。";
  }
  std::wstring out;
  out += L"被控端详情\r\n";
  out += L"────────────────\r\n";
  out += L"显示名称：";
  out += device->name;
  out += L"\r\n";
  out += L"状态：";
  out += device->role.empty() ? (device->online ? L"在线" : L"离线") : device->role;
  out += L"\r\n";
  if (!device->computer_role.empty()) {
    out += L"设备角色：";
    out += device->computer_role;
    out += L"\r\n";
  }
  if (!device->install_location.empty()) {
    out += L"安装位置：";
    out += device->install_location;
    out += L"\r\n";
  }
  if (!device->lane_number.empty()) {
    out += L"车道编号：";
    out += device->lane_number;
    out += L"\r\n";
  }
  if (!device->agent_id.empty()) {
    out += L"Agent ID：";
    out += utf8_to_wide(device->agent_id);
    out += L"\r\n";
  }
  if (!device->hostname.empty()) {
    out += L"主机名：";
    out += utf8_to_wide(device->hostname);
    out += L"\r\n";
  }
  out += L"媒体地址：";
  if (!device->host.empty()) {
    out += utf8_to_wide(device->host);
  } else {
    out += L"—";
  }
  out += L":";
  out += std::to_wstring(device->port > 0 ? device->port : 38471);
  out += L"\r\n";
  if (!device->ipv4s.empty()) {
    out += L"网卡 IPv4：";
    out += utf8_to_wide(device->ipv4s);
    out += L"\r\n";
  }
  out += L"Host Agent 版本：";
  out += device->version.empty() ? L"—\r\n" : (utf8_to_wide(device->version) + L"\r\n");
  if (!device->last_seen.empty()) {
    out += L"最近心跳：";
    out += utf8_to_wide(device->last_seen);
    out += L"\r\n";
  }
  if (!device->tags.empty()) {
    out += L"标签：";
    out += utf8_to_wide(device->tags);
    out += L"\r\n";
  }
  if (!device->remark.empty() && device->remark.rfind(L"gid=", 0) != 0) {
    out += L"备注：";
    out += device->remark;
    out += L"\r\n";
  }
  out += L"\r\n计算机与磁盘\r\n";
  out += L"────────────────\r\n";
  out += format_inventory_section(device->inventory_json);
  return out;
}

}  // namespace road_desk::viewer
