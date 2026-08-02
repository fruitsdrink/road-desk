#include "gateway_config.h"

#include "log.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>

namespace road_desk::agent {
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
  size_t end = json.find('"', p + 1);
  if (end == std::string::npos) {
    return {};
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

std::string new_agent_id() {
  GUID g{};
  if (CoCreateGuid(&g) != S_OK) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "agent-%08lx", GetTickCount());
    return buf;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                static_cast<unsigned long>(g.Data1), g.Data2, g.Data3, g.Data4[0], g.Data4[1],
                g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  return buf;
}

struct DlgState {
  GatewayConfig* cfg = nullptr;
  HWND host = nullptr;
  HWND port = nullptr;
  HWND psk = nullptr;
  bool ok = false;
  bool done = false;
};

DlgState* g_dlg = nullptr;

LRESULT CALLBACK CfgWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  DlgState* st = g_dlg;
  switch (msg) {
    case WM_COMMAND:
      if (!st) {
        break;
      }
      if (LOWORD(wparam) == IDOK) {
        char host[256] = {};
        char port[32] = {};
        char psk[512] = {};
        GetWindowTextA(st->host, host, sizeof(host));
        GetWindowTextA(st->port, port, sizeof(port));
        GetWindowTextA(st->psk, psk, sizeof(psk));
        std::string h = trim(host);
        std::string p = trim(port);
        std::string k = trim(psk);
        if (h.empty() || p.empty() || k.empty()) {
          MessageBoxW(hwnd, L"请填写网关地址、端口与 Agent PSK", L"Road Desk", MB_OK | MB_ICONWARNING);
          return 0;
        }
        st->cfg->gateway_url = "http://" + h + ":" + p;
        st->cfg->agent_psk = k;
        if (st->cfg->agent_id.empty()) {
          st->cfg->agent_id = new_agent_id();
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

std::string gateway_config_path() {
  char prog[MAX_PATH] = {};
  if (FAILED(SHGetFolderPathA(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, prog))) {
    return "agent.json";
  }
  return std::string(prog) + "\\RoadDesk\\agent.json";
}

bool load_gateway_config(GatewayConfig* out) {
  if (!out) {
    return false;
  }
  const std::string path = gateway_config_path();
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  out->agent_id = json_get_string(json, "agentId");
  out->gateway_url = json_get_string(json, "gatewayUrl");
  out->agent_psk = json_get_string(json, "agentPsk");
  return !out->agent_id.empty() && !out->gateway_url.empty() && !out->agent_psk.empty();
}

bool save_gateway_config(const GatewayConfig& cfg) {
  const std::string path = gateway_config_path();
  const size_t slash = path.find_last_of("\\/");
  if (slash != std::string::npos) {
    CreateDirectoryA(path.substr(0, slash).c_str(), nullptr);
  }
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    log_line("gateway: failed to write agent.json");
    return false;
  }
  out << "{\n"
      << "  \"agentId\": \"" << json_escape(cfg.agent_id) << "\",\n"
      << "  \"gatewayUrl\": \"" << json_escape(cfg.gateway_url) << "\",\n"
      << "  \"agentPsk\": \"" << json_escape(cfg.agent_psk) << "\"\n"
      << "}\n";
  return true;
}

bool prompt_gateway_config(GatewayConfig* out) {
  if (!out) {
    return false;
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  const wchar_t* kClass = L"RoadDeskGatewayCfg";
  WNDCLASSW wc{};
  wc.lpfnWndProc = CfgWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kClass;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  RegisterClassW(&wc);

  DlgState st;
  st.cfg = out;
  g_dlg = &st;

  HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"Road Desk — 网关配置",
                              WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT,
                              CW_USEDEFAULT, 440, 280, nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) {
    g_dlg = nullptr;
    return false;
  }

  CreateWindowW(L"STATIC", L"网关 IP / 主机", WS_CHILD | WS_VISIBLE, 20, 20, 160, 20, hwnd, nullptr,
                wc.hInstance, nullptr);
  st.host = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "127.0.0.1",
                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 20, 42, 380, 24, hwnd, nullptr,
                            wc.hInstance, nullptr);
  CreateWindowW(L"STATIC", L"端口", WS_CHILD | WS_VISIBLE, 20, 76, 80, 20, hwnd, nullptr, wc.hInstance,
                nullptr);
  st.port = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "8743", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                            20, 98, 120, 24, hwnd, nullptr, wc.hInstance, nullptr);
  CreateWindowW(L"STATIC", L"Agent PSK", WS_CHILD | WS_VISIBLE, 20, 132, 120, 20, hwnd, nullptr,
                wc.hInstance, nullptr);
  st.psk = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 20,
                           154, 280, 24, hwnd, nullptr, wc.hInstance, nullptr);
  CreateWindowW(L"BUTTON", L"选文件…", WS_CHILD | WS_VISIBLE, 310, 154, 90, 24, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001)), wc.hInstance, nullptr);
  CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 220, 200, 80, 28, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), wc.hInstance, nullptr);
  CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, 320, 200, 80, 28, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), wc.hInstance, nullptr);

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

}  // namespace road_desk::agent
