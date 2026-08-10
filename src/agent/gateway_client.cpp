#include "gateway_client.h"

#include "common/http_helpers.h"
#include "host_inventory.h"
#include "log.h"

#include <cstdio>
#include <cstring>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <iphlpapi.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace road_desk::agent {
namespace {

bool is_loopback_or_linklocal(const std::string& ip) {
  if (ip.rfind("127.", 0) == 0) {
    return true;
  }
  if (ip.rfind("169.254.", 0) == 0) {
    return true;
  }
  if (ip == "0.0.0.0") {
    return true;
  }
  return false;
}

std::string wide_to_utf8(const std::wstring& w) {
  if (w.empty()) {
    return {};
  }
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
  if (n > 1) {
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  }
  return s;
}

// SEH filter for the gateway thread. Returns EXCEPTION_EXECUTE_HANDLER after
// logging the faulting code + address + module, so the __except handler body runs.
int log_gateway_seh(EXCEPTION_POINTERS* info) {
  const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
  const void* addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress
                                                   : nullptr;
  char mod_name[MAX_PATH] = "unknown";
  HMODULE mod = nullptr;
  if (addr &&
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(addr), &mod) &&
      mod) {
    GetModuleFileNameA(mod, mod_name, MAX_PATH);
  }
  char line[320];
  std::snprintf(line, sizeof(line), "gateway: thread SEH exception code=0x%08lx addr=%p module=%s",
                static_cast<unsigned long>(code), addr, mod_name);
  log_line(line);
  return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

GatewayClient::~GatewayClient() {
  stop();
}

void GatewayClient::start(const GatewayConfig& cfg, int media_port, const std::string& version) {
  stop();
  cfg_ = cfg;
  media_port_ = media_port;
  version_ = version;
  stop_ = false;
  thr_ = std::thread([this] { gateway_thread_main(); });
}

void GatewayClient::stop() {
  stop_ = true;
  if (thr_.joinable()) {
    thr_.join();
  }
}

std::vector<std::string> GatewayClient::list_ipv4s() const {
  std::vector<std::string> out;
  ULONG sz = 15000;
  std::vector<char> buf(sz);
  auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  DWORD flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  DWORD rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &sz);
  if (rc == ERROR_BUFFER_OVERFLOW) {
    buf.resize(sz);
    addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &sz);
  }
  if (rc != NO_ERROR) {
    return out;
  }
  for (IP_ADAPTER_ADDRESSES* a = addrs; a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp) {
      continue;
    }
    for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
      if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) {
        continue;
      }
      auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
      char ip[INET_ADDRSTRLEN] = {};
      if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) {
        std::string s(ip);
        if (!is_loopback_or_linklocal(s)) {
          out.push_back(s);
        }
      }
    }
  }
  return out;
}

std::string GatewayClient::preferred_ipv4() const {
  std::wstring host;
  unsigned short port = 0;
  bool https = false;
  if (!road_desk::common::parse_url(cfg_.gateway_url, &host, &port, &https, nullptr)) {
    return {};
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string host_a = wide_to_utf8(host);
  char port_s[16];
  std::snprintf(port_s, sizeof(port_s), "%u", static_cast<unsigned>(port));
  if (getaddrinfo(host_a.c_str(), port_s, &hints, &res) != 0 || !res) {
    return {};
  }
  SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  std::string local;
  if (s != INVALID_SOCKET) {
    // short connect timeout via nonblocking
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(s, &wset);
    timeval tv{2, 0};
    if (select(0, nullptr, &wset, nullptr, &tv) > 0) {
      sockaddr_in name{};
      int namelen = sizeof(name);
      if (getsockname(s, reinterpret_cast<sockaddr*>(&name), &namelen) == 0) {
        char buf[64] = {};
        inet_ntop(AF_INET, &name.sin_addr, buf, sizeof(buf));
        local = buf;
      }
    }
    closesocket(s);
  }
  freeaddrinfo(res);
  if (is_loopback_or_linklocal(local)) {
    return {};
  }
  return local;
}

std::string GatewayClient::build_body() const {
  char hostname[256] = {};
  DWORD n = sizeof(hostname);
  GetComputerNameA(hostname, &n);
  auto ips = list_ipv4s();
  std::string preferred = preferred_ipv4();
  if (preferred.empty() && !ips.empty()) {
    preferred = ips.front();
  }
  std::ostringstream ss;
  ss << "{\"agentId\":\"" << cfg_.agent_id << "\","
     << "\"hostname\":\"" << hostname << "\","
     << "\"version\":\"" << version_ << "\","
     << "\"mediaPort\":" << media_port_ << ","
     << "\"preferredIpv4\":\"" << preferred << "\","
     << "\"ipv4s\":[";
  for (size_t i = 0; i < ips.size(); ++i) {
    if (i) {
      ss << ',';
    }
    ss << '"' << ips[i] << '"';
  }
  ss << "],"
     << "\"inventory\":" << collect_host_inventory_json() << '}';
  return ss.str();
}

bool GatewayClient::post_json(const char* path, const std::string& body) {
  // Use common::http_post_json for the actual HTTP call — we inline the auth header
  // construction here because GatewayClient already owns the Bearer assembly.
  std::string err;
  return road_desk::common::http_post_json(cfg_.gateway_url, path, cfg_.agent_psk, body, 0, &err);
}

void GatewayClient::gateway_thread_main() {
  // SEH guard: an access violation in this background thread would otherwise
  // terminate the whole process without any log (SetUnhandledExceptionFilter
  // only covers the main thread). Log the faulting address, then end the thread.
  __try {
    run();
  } __except (log_gateway_seh(GetExceptionInformation())) {
    // handler already logged; thread ends here
  }
}

void GatewayClient::run() {
  // Do not WSAStartup/WSACleanup here — media plane owns Winsock lifetime.
  // A mismatched WSACleanup from this thread tears down sockets under serve().
  try {
    gateway_run_loop();
  } catch (const std::exception& ex) {
    log_line(std::string("gateway: thread FATAL exception: ") + ex.what());
  } catch (...) {
    log_line("gateway: thread FATAL unknown exception");
  }
}

void GatewayClient::gateway_run_loop() {
  bool registered = false;
  while (!stop_) {
    const std::string body = build_body();
    if (!registered) {
      if (post_json("/v1/agents/register", body)) {
        registered = true;
        log_line("gateway: registered");
      } else {
        log_line("gateway: register failed — retry in 5s");
      }
    } else {
      if (!post_json("/v1/agents/heartbeat", body)) {
        log_line("gateway: heartbeat failed — will retry");
        registered = false;
      }
    }
    for (int i = 0; i < 15 && !stop_; ++i) {
      Sleep(1000);
    }
  }
}

}  // namespace road_desk::agent
