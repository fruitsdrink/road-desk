#include "gateway_config.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace road_desk::viewer {
namespace {

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
    s.pop_back();
  }
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
    ++i;
  }
  return s.substr(i);
}

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

std::string json_get_string(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) {
    return {};
  }
  p = json.find(':', p + needle.size());
  if (p == std::string::npos) {
    return {};
  }
  p = json.find('"', p + 1);
  if (p == std::string::npos) {
    return {};
  }
  size_t end = p + 1;
  while (end < json.size()) {
    if (json[end] == '\\' && end + 1 < json.size()) {
      end += 2;
      continue;
    }
    if (json[end] == '"') {
      break;
    }
    ++end;
  }
  return json.substr(p + 1, end - p - 1);
}

std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '\\' || c == '"') {
      o.push_back('\\');
    }
    o.push_back(c);
  }
  return o;
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
  while (!path_prefix->empty() && path_prefix->back() == L'/') {
    path_prefix->pop_back();
  }
  return !host->empty();
}

bool split_gateway_url(const std::string& url, std::string* host, std::string* port) {
  std::wstring whost;
  INTERNET_PORT p = 0;
  bool https = false;
  std::wstring prefix;
  if (!parse_url(url, &whost, &p, &https, &prefix)) {
    return false;
  }
  char narrow[256] = {};
  WideCharToMultiByte(CP_UTF8, 0, whost.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
  *host = narrow;
  char port_buf[16];
  std::snprintf(port_buf, sizeof(port_buf), "%u", static_cast<unsigned>(p));
  *port = port_buf;
  return true;
}

bool http_post_json(const std::string& base_url, const std::string& path, const std::string& json_body,
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
  HINTERNET req = WinHttpOpenRequest(con, L"POST", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!req) {
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    *err = "open request failed";
    return false;
  }
  std::string payload = json_body;
  BOOL ok = WinHttpSendRequest(req, L"Content-Type: application/json\r\n", static_cast<DWORD>(-1),
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
  if (!ok || status < 200 || status >= 300) {
    std::string msg = json_get_string(out, "error");
    if (msg.empty()) {
      char line[64];
      std::snprintf(line, sizeof(line), "HTTP %lu", static_cast<unsigned long>(status));
      msg = line;
    }
    *err = msg;
    return false;
  }
  *body = std::move(out);
  return true;
}

struct DlgState {
  DirectoryConfig* cfg = nullptr;
  HWND host = nullptr;
  HWND port = nullptr;
  HWND user = nullptr;
  HWND pass = nullptr;
  HWND psk = nullptr;
  bool ok = false;
  bool done = false;
};

DlgState* g_dlg = nullptr;

bool apply_dialog(DlgState* st, HWND hwnd) {
  char host[256] = {};
  char port[32] = {};
  char psk[2048] = {};
  wchar_t user_w[128] = {};
  wchar_t pass_w[256] = {};
  GetWindowTextA(st->host, host, sizeof(host));
  GetWindowTextA(st->port, port, sizeof(port));
  GetWindowTextW(st->user, user_w, 128);
  GetWindowTextW(st->pass, pass_w, 256);
  GetWindowTextA(st->psk, psk, sizeof(psk));
  std::string h = trim(host);
  std::string p = trim(port);
  char user_u8[384] = {};
  char pass_u8[768] = {};
  WideCharToMultiByte(CP_UTF8, 0, user_w, -1, user_u8, sizeof(user_u8), nullptr, nullptr);
  WideCharToMultiByte(CP_UTF8, 0, pass_w, -1, pass_u8, sizeof(pass_u8), nullptr, nullptr);
  std::string u = trim(user_u8);
  std::string pw = trim(pass_u8);
  std::string k = trim(psk);
  if (h.empty() || p.empty()) {
    MessageBoxW(hwnd, L"请填写网关地址与端口", L"Road Desk", MB_OK | MB_ICONWARNING);
    return false;
  }
  const std::string url = "http://" + h + ":" + p;
  if (!k.empty()) {
    st->cfg->gateway_url = url;
    st->cfg->directory_key = k;
    if (!u.empty()) {
      st->cfg->username = u;
    }
    return true;
  }
  if (u.empty() || pw.empty()) {
    MessageBoxW(hwnd, L"请填写帐号密码，或导入 Viewer PSK", L"Road Desk", MB_OK | MB_ICONWARNING);
    return false;
  }
  std::string token;
  std::string err;
  if (!viewer_login(url, u, pw, &token, &err)) {
    MessageBoxW(hwnd, (L"登录失败：" + utf8_to_wide(err)).c_str(), L"Road Desk", MB_OK | MB_ICONERROR);
    return false;
  }
  st->cfg->gateway_url = url;
  st->cfg->directory_key = token;
  st->cfg->username = u;
  return true;
}

LRESULT CALLBACK CfgWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  DlgState* st = g_dlg;
  switch (msg) {
    case WM_COMMAND:
      if (!st) {
        break;
      }
      if (LOWORD(wparam) == IDOK) {
        if (!apply_dialog(st, hwnd)) {
          return 0;
        }
        st->ok = true;
        st->done = true;
        DestroyWindow(hwnd);
        return 0;
      }
      if (LOWORD(wparam) == IDCANCEL) {
        st->ok = false;
        st->done = true;
        DestroyWindow(hwnd);
        return 0;
      }
      if (LOWORD(wparam) == 1001) {
        char path[MAX_PATH] = {};
        OPENFILENAMEA ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = "PSK Files\0*.psk;*.txt\0All\0*.*\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameA(&ofn)) {
          std::ifstream in(path);
          std::string line;
          std::getline(in, line);
          line = trim(line);
          if (!line.empty()) {
            SetWindowTextA(st->psk, line.c_str());
          }
        }
        return 0;
      }
      break;
    case WM_CLOSE:
      if (st) {
        st->ok = false;
        st->done = true;
      }
      DestroyWindow(hwnd);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

std::string directory_config_path() {
  char prog[MAX_PATH] = {};
  if (FAILED(SHGetFolderPathA(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, prog))) {
    return "viewer.json";
  }
  return std::string(prog) + "\\RoadDesk\\viewer.json";
}

DirectoryConfig load_directory_config() {
  DirectoryConfig c;
  c.gateway_url = env_str("ROAD_DESK_GATEWAY_URL");
  c.directory_key = env_str("ROAD_DESK_VIEWER_KEY");
  if (c.directory_key.empty()) {
    c.directory_key = env_str("ROAD_DESK_DIRECTORY_KEY");
  }
  c.username = env_str("ROAD_DESK_VIEWER_USER");

  const std::string path = directory_config_path();
  std::ifstream in(path);
  if (in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string json = ss.str();
    if (c.gateway_url.empty()) {
      c.gateway_url = json_get_string(json, "gatewayUrl");
    }
    if (c.directory_key.empty()) {
      c.directory_key = json_get_string(json, "viewerKey");
      if (c.directory_key.empty()) {
        c.directory_key = json_get_string(json, "directoryKey");
      }
    }
    if (c.username.empty()) {
      c.username = json_get_string(json, "username");
    }
  }
  return c;
}

bool save_directory_config(const DirectoryConfig& cfg) {
  const std::string path = directory_config_path();
  const size_t slash = path.find_last_of("\\/");
  if (slash != std::string::npos) {
    CreateDirectoryA(path.substr(0, slash).c_str(), nullptr);
  }
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return false;
  }
  out << "{\n"
      << "  \"gatewayUrl\": \"" << json_escape(cfg.gateway_url) << "\",\n"
      << "  \"viewerKey\": \"" << json_escape(cfg.directory_key) << "\"";
  if (!cfg.username.empty()) {
    out << ",\n  \"username\": \"" << json_escape(cfg.username) << "\"";
  }
  out << "\n}\n";
  return true;
}

bool viewer_login(const std::string& gateway_url, const std::string& username,
                  const std::string& password, std::string* token, std::string* err) {
  if (!token) {
    return false;
  }
  const std::string body =
      std::string("{\"username\":\"") + json_escape(username) + "\",\"password\":\"" +
      json_escape(password) + "\"}";
  std::string resp;
  std::string e;
  if (!http_post_json(gateway_url, "/v1/viewer/login", body, &resp, &e)) {
    if (err) {
      *err = e;
    }
    return false;
  }
  *token = json_get_string(resp, "token");
  if (token->empty()) {
    if (err) {
      *err = "login response missing token";
    }
    return false;
  }
  return true;
}

bool prompt_directory_config(DirectoryConfig* inout) {
  if (!inout) {
    return false;
  }

  std::string pre_host = "127.0.0.1";
  std::string pre_port = "8743";
  if (!inout->gateway_url.empty()) {
    std::string h, p;
    if (split_gateway_url(inout->gateway_url, &h, &p)) {
      pre_host = h;
      pre_port = p;
    }
  }

  const wchar_t* kClass = L"RoadDeskViewerGatewayCfg";
  WNDCLASSW wc{};
  wc.lpfnWndProc = CfgWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kClass;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  RegisterClassW(&wc);

  DlgState st;
  st.cfg = inout;
  g_dlg = &st;

  HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"Road Desk — Viewer 登录",
                              WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT,
                              CW_USEDEFAULT, 460, 420, nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) {
    g_dlg = nullptr;
    return false;
  }

  CreateWindowW(L"STATIC", L"网关 IP / 主机", WS_CHILD | WS_VISIBLE, 20, 16, 160, 20, hwnd, nullptr,
                wc.hInstance, nullptr);
  st.host = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", pre_host.c_str(),
                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, 20, 38, 400, 24, hwnd,
                            nullptr, wc.hInstance, nullptr);
  CreateWindowW(L"STATIC", L"端口", WS_CHILD | WS_VISIBLE, 20, 70, 80, 20, hwnd, nullptr, wc.hInstance,
                nullptr);
  st.port = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", pre_port.c_str(),
                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, 20, 92, 120, 24, hwnd,
                            nullptr, wc.hInstance, nullptr);

  CreateWindowW(L"STATIC", L"帐号登录（操作员）", WS_CHILD | WS_VISIBLE, 20, 130, 200, 20, hwnd,
                nullptr, wc.hInstance, nullptr);
  CreateWindowW(L"STATIC", L"用户名", WS_CHILD | WS_VISIBLE, 20, 154, 80, 20, hwnd, nullptr,
                wc.hInstance, nullptr);
  st.user = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", utf8_to_wide(inout->username).c_str(),
                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, 20, 176, 400, 24,
                            hwnd, nullptr, wc.hInstance, nullptr);
  CreateWindowW(L"STATIC", L"密码", WS_CHILD | WS_VISIBLE, 20, 208, 80, 20, hwnd, nullptr,
                wc.hInstance, nullptr);
  st.pass = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, 20,
                            230, 400, 24, hwnd, nullptr, wc.hInstance, nullptr);

  CreateWindowW(L"STATIC", L"或导入 Viewer PSK", WS_CHILD | WS_VISIBLE, 20, 268, 200, 20, hwnd,
                nullptr, wc.hInstance, nullptr);
  st.psk = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, 20, 290, 300, 24,
                           hwnd, nullptr, wc.hInstance, nullptr);
  CreateWindowW(L"BUTTON", L"选文件…", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 330, 290, 90, 24, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001)), wc.hInstance, nullptr);

  CreateWindowW(L"BUTTON", L"登录", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 230, 340,
                90, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), wc.hInstance,
                nullptr);
  CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 330, 340, 90, 28, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), wc.hInstance, nullptr);

  // Pre-fill PSK only when it looks like a short key (not a JWT).
  if (!inout->directory_key.empty() && inout->directory_key.find('.') == std::string::npos) {
    SetWindowTextA(st.psk, inout->directory_key.c_str());
  }

  MSG msg;
  while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  g_dlg = nullptr;
  return st.ok;
}

}  // namespace road_desk::viewer
