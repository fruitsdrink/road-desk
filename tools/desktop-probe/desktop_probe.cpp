// desktop_probe.exe — verify whether a process can access the Winlogon
// secure desktop (lock screen) and BitBlt it.
//
// Run from an interactive-session process (admin or SYSTEM) while the host is
// locked. Reports, step by step, whether OpenDesktop("Winlogon") /
// OpenInputDesktop succeed and whether GDI capture on that desktop works.
//
// Build (MSVC, x64):
//   cl /nologo /O2 /MT desktop_probe.cpp /Fe:desktop_probe.exe user32.lib gdi32.lib
//
// Exit codes: 0 = all steps OK, nonzero otherwise. Prints diagnostics to stdout.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wtsapi32.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static void Out(const char* fmt, ...);

static void Step(const char* name, BOOL ok, DWORD err) {
  Out("[%s] %s => %s%s\n", ok ? "OK " : "FAIL", name, ok ? "success" : "failed",
      ok ? "" : " (GetLastError=... )");
  if (!ok) {
    Out("      err=%lu (0x%08lx)\n", err, err);
  }
}

// Also mirror all stdout to desktop_probe.txt so results are readable when
// running from a non-interactive context (task scheduler / SYSTEM).
static FILE* g_log = nullptr;
static void Out(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  _vsnprintf_s(buf, _TRUNCATE, fmt, ap);
  va_end(ap);
  std::printf("%s", buf);
  if (g_log) {
    std::fputs(buf, g_log);
    std::fflush(g_log);
  }
}

static bool RunningAsAdmin() {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
    return false;
  }
  TOKEN_ELEVATION elev{};
  DWORD got = 0;
  const BOOL ok = GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &got);
  CloseHandle(tok);
  return ok && elev.TokenIsElevated;
}

// Test access to another session's interactive window station (WinSta0) and its
// Winlogon desktop. Session 0 (service) has no interactive station by default.
static std::string wchar_to_utf8(const wchar_t* w) {
  if (!w || !w[0]) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
  if (n > 1) {
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  }
  return s;
}

static void TestCrossSessionStation(DWORD target_session) {
  wchar_t ws_path[128];
  swprintf_s(ws_path, L"\\Sessions\\%lu\\Windows\\WindowStations\\WinSta0", target_session);
  Out("--- cross-session station test (target session %lu) ---\n", target_session);

  HWINSTA hw = OpenWindowStationW(ws_path, FALSE,
                                  WINSTA_READATTRIBUTES | WINSTA_ENUMDESKTOPS |
                                      WINSTA_ACCESSCLIPBOARD | WINSTA_ACCESSGLOBALATOMS |
                                      WINSTA_CREATEDESKTOP | WINSTA_WRITEATTRIBUTES);
  Step(("OpenWindowStation(" + wchar_to_utf8(ws_path) + ")").c_str(), hw != nullptr,
       GetLastError());
  if (!hw) {
    return;
  }
  HWINSTA old = GetProcessWindowStation();
  Step("SetProcessWindowStation(cross-session)", SetProcessWindowStation(hw), GetLastError());

  HDESK hWinlogon = OpenDesktopW(L"Winlogon", 0, FALSE,
                                 DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
                                     DESKTOP_SWITCHDESKTOP);
  Step("OpenDesktop(Winlogon) after switch", hWinlogon != nullptr, GetLastError());
  if (hWinlogon) {
    // Try to capture from the Winlogon desktop's DC.
    HDC dc = GetDC(nullptr);
    if (dc) {
      const int w = GetSystemMetrics(SM_CXSCREEN);
      const int hh = GetSystemMetrics(SM_CYSCREEN);
      HDC mem = CreateCompatibleDC(dc);
      BITMAPINFO bmi{};
      bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bmi.bmiHeader.biWidth = w;
      bmi.bmiHeader.biHeight = -hh;
      bmi.bmiHeader.biPlanes = 1;
      bmi.bmiHeader.biBitCount = 32;
      bmi.bmiHeader.biCompression = BI_RGB;
      void* bits = nullptr;
      HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
      HGDIOBJ oldbmp = SelectObject(mem, dib);
      const BOOL blt = BitBlt(mem, 0, 0, w, hh, dc, 0, 0, SRCCOPY);
      Step("BitBlt(cross-session Winlogon)", blt, GetLastError());
      Out("      screen=%dx%d\n", w, hh);
      uint32_t nonzero = 0;
      if (blt && bits) {
        const uint32_t* px = static_cast<const uint32_t*>(bits);
        const size_t n = static_cast<size_t>(w) * static_cast<size_t>(hh);
        const size_t stride = n > 65536 ? n / 65536 : 1;
        for (size_t i = 0; i < n; i += stride) {
          if (px[i] != 0) {
            ++nonzero;
          }
        }
        Out("      sampled non-black pixels: %u / ~%u\n",
            static_cast<unsigned>(nonzero), static_cast<unsigned>(n / stride));
      }
      SelectObject(mem, oldbmp);
      DeleteObject(dib);
      DeleteDC(mem);
      ReleaseDC(nullptr, dc);
    } else {
      Step("GetDC for cross-session Winlogon", FALSE, GetLastError());
    }
    CloseDesktop(hWinlogon);
  }
  SetProcessWindowStation(old);
  CloseWindowStation(hw);
}

// Runs inside the injected child (SYSTEM token + target session). Probes whether
// the Winlogon secure desktop is reachable and capturable from that context.
static void RunProbeChild() {
  Out("=== desktop_probe (injected child) ===\n");
  Out("process elevated admin=%d\n", RunningAsAdmin() ? 1 : 0);
  {
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
      TOKEN_ELEVATION elev{};
      DWORD got = 0;
      if (GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &got)) {
        Out("token elevated: %d\n", elev.TokenIsElevated ? 1 : 0);
      }
      CloseHandle(tok);
    }
  }
  const DWORD csid = WTSGetActiveConsoleSessionId();
  Out("active console session (from child): %lu\n", csid);
  {
    HWINSTA ws = GetProcessWindowStation();
    wchar_t ws_name[128] = {};
    DWORD need = 0;
    if (ws && GetUserObjectInformationW(ws, UOI_NAME, ws_name, sizeof(ws_name), &need)) {
      Out("child window station: %ls\n", ws_name);
    }
  }

  HDESK hWinlogon = OpenDesktopW(L"Winlogon", 0, FALSE,
                                 DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
                                     DESKTOP_SWITCHDESKTOP);
  Step("child OpenDesktop(Winlogon)", hWinlogon != nullptr, GetLastError());
  HDESK hDefault = OpenDesktopW(L"Default", 0, FALSE,
                                DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
                                    DESKTOP_SWITCHDESKTOP);
  Step("child OpenDesktop(Default)", hDefault != nullptr, GetLastError());
  if (hDefault) {
    CloseDesktop(hDefault);
  }
  HDESK hInput = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS);
  Step("child OpenInputDesktop", hInput != nullptr, GetLastError());
  if (hInput) {
    wchar_t name[256] = {};
    DWORD need = 0;
    if (GetUserObjectInformationW(hInput, UOI_NAME, name, sizeof(name), &need)) {
      Out("      child input desktop: %ls\n", name);
    }
    CloseDesktop(hInput);
  }

  if (hWinlogon) {
    HDC dc = GetDC(nullptr);
    if (dc) {
      const int w = GetSystemMetrics(SM_CXSCREEN);
      const int hh = GetSystemMetrics(SM_CYSCREEN);
      HDC mem = CreateCompatibleDC(dc);
      BITMAPINFO bmi{};
      bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bmi.bmiHeader.biWidth = w;
      bmi.bmiHeader.biHeight = -hh;
      bmi.bmiHeader.biPlanes = 1;
      bmi.bmiHeader.biBitCount = 32;
      bmi.bmiHeader.biCompression = BI_RGB;
      void* bits = nullptr;
      HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
      HGDIOBJ oldbmp = SelectObject(mem, dib);
      const BOOL blt = BitBlt(mem, 0, 0, w, hh, dc, 0, 0, SRCCOPY);
      Step("child BitBlt(Winlogon)", blt, GetLastError());
      Out("      screen=%dx%d\n", w, hh);
      uint32_t nonzero = 0;
      if (blt && bits) {
        const uint32_t* px = static_cast<const uint32_t*>(bits);
        const size_t n = static_cast<size_t>(w) * static_cast<size_t>(hh);
        const size_t stride = n > 65536 ? n / 65536 : 1;
        for (size_t i = 0; i < n; i += stride) {
          if (px[i] != 0) {
            ++nonzero;
          }
        }
        Out("      sampled non-black pixels: %u / ~%u\n",
            static_cast<unsigned>(nonzero), static_cast<unsigned>(n / stride));
      }
      SelectObject(mem, oldbmp);
      DeleteObject(dib);
      DeleteDC(mem);
      ReleaseDC(nullptr, dc);
    } else {
      Step("child GetDC for Winlogon", FALSE, GetLastError());
    }
    CloseDesktop(hWinlogon);
  }
  Out("=== child done ===\n");
}

// Service side: build a SYSTEM token bound to the target session and launch this
// exe with --probe-desktop as that token, so the child runs in the interactive
// session under SYSTEM — the only context that may reach the Winlogon desktop.
static void InjectIntoSession(DWORD target_session) {
  Out("--- inject into session %lu (SYSTEM token) ---\n", target_session);

  HANDLE proc_tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY |
                                                 TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT |
                                                 TOKEN_ADJUST_SESSIONID,
                        &proc_tok)) {
    Step("OpenProcessToken(SYSTEM)", FALSE, GetLastError());
    return;
  }
  // Changing TokenSessionId requires SeTcbPrivilege (Act as part of the OS).
  // LocalSystem holds it but disabled; enable it via AdjustTokenPrivileges.
  {
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, L"SeTcbPrivilege", &tp.Privileges[0].Luid)) {
      Step("LookupPrivilegeValue(SeTcb)", FALSE, GetLastError());
      CloseHandle(proc_tok);
      return;
    }
    // Probe whether SeTcb is present in the token at all (even if disabled).
    {
      DWORD got = 0;
      BOOL present = FALSE;
      if (GetTokenInformation(proc_tok, TokenPrivileges, nullptr, 0, &got) ||
          GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<char> buf(got);
        if (GetTokenInformation(proc_tok, TokenPrivileges, buf.data(), got, &got)) {
          const auto* privs = reinterpret_cast<const TOKEN_PRIVILEGES*>(buf.data());
          for (DWORD i = 0; i < privs->PrivilegeCount; ++i) {
            if (privs->Privileges[i].Luid.LowPart == tp.Privileges[0].Luid.LowPart &&
                privs->Privileges[i].Luid.HighPart == tp.Privileges[0].Luid.HighPart) {
              present = TRUE;
              Out("      SeTcb present, attributes=0x%08lx\n",
                  static_cast<unsigned long>(privs->Privileges[i].Attributes));
              break;
            }
          }
        }
      }
      Out("      SeTcb present in token: %s\n", present ? "yes" : "no");
    }
    SetLastError(0);
    const BOOL adj = AdjustTokenPrivileges(proc_tok, FALSE, &tp, 0, nullptr, nullptr);
    const DWORD adj_err = GetLastError();
    Step("AdjustTokenPrivileges(SeTcb)", adj && adj_err == ERROR_SUCCESS, adj_err);
  }
  // Set the token's session id so the child lands in the target session.
  if (!SetTokenInformation(proc_tok, TokenSessionId, &target_session,
                           sizeof(target_session))) {
    Step("SetTokenInformation(TokenSessionId)", FALSE, GetLastError());
    CloseHandle(proc_tok);
    return;
  }
  Step("SetTokenInformation(TokenSessionId)", TRUE, 0);

  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring cmd = std::wstring(L"\"") + exe + L"\" --probe-desktop";

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessAsUserW(proc_tok, nullptr, &cmd[0], nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    Step("CreateProcessAsUser(child)", FALSE, GetLastError());
    CloseHandle(proc_tok);
    return;
  }
  Step("CreateProcessAsUser(child)", TRUE, 0);
  CloseHandle(pi.hThread);
  // Wait for the child to finish probing and write desktop_probe.txt.
  WaitForSingleObject(pi.hProcess, 20000);
  CloseHandle(pi.hProcess);
  CloseHandle(proc_tok);
  Out("--- inject done ---\n");
}

// Run the probe in a LocalSystem service so we can test from Session 0 as
// SYSTEM (task-scheduled SYSTEM runs non-interactive and can't reach WinSta0).
static SERVICE_STATUS g_svc_status{};
static SERVICE_STATUS_HANDLE g_svc_handle = nullptr;
static DWORD WINAPI SvcCtrlHandler(DWORD, DWORD, LPVOID, LPVOID) {
  return NO_ERROR;
}
static void WINAPI SvcMain(DWORD, wchar_t**) {
  g_svc_handle = RegisterServiceCtrlHandlerExW(L"desktop_probe", SvcCtrlHandler, nullptr);
  g_svc_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_svc_status.dwCurrentState = SERVICE_RUNNING;
  SetServiceStatus(g_svc_handle, &g_svc_status);

  Out("=== desktop_probe (service mode) ===\n");
  Out("process elevated admin=%d\n", RunningAsAdmin() ? 1 : 0);
  {
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
      BYTE buf[64] = {};
      DWORD got = 0;
      if (GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof(buf), &got)) {
        PSID sid = reinterpret_cast<PSID>(buf + sizeof(DWORD));
        char name[64] = {};
        char domain[64] = {};
        DWORD n = sizeof(name);
        DWORD d = sizeof(domain);
        SID_NAME_USE use = SidTypeUnknown;
        if (LookupAccountSidA(nullptr, sid, name, &n, domain, &d, &use)) {
          Out("integrity level: %s\\%s\n", domain, name);
        }
      }
      CloseHandle(tok);
    }
  }
  const DWORD csid = WTSGetActiveConsoleSessionId();
  Out("active console session: %lu\n", csid);
  // Current window station (Session 0 services usually run in "Service-0x0-...$").
  {
    HWINSTA ws = GetProcessWindowStation();
    wchar_t ws_name[128] = {};
    DWORD need = 0;
    if (ws && GetUserObjectInformationW(ws, UOI_NAME, ws_name, sizeof(ws_name), &need)) {
      Out("current window station: %ls\n", ws_name);
    }
  }
  // Test opening the console session's WinSta0 + Winlogon.
  TestCrossSessionStation(csid);
  // The real test: inject a SYSTEM-token child into the interactive session.
  InjectIntoSession(csid);

  g_svc_status.dwCurrentState = SERVICE_STOPPED;
  SetServiceStatus(g_svc_handle, &g_svc_status);
  if (g_log) {
    std::fclose(g_log);
    g_log = nullptr;
  }
}

int main(int argc, char** argv) {
  // Child mode: injected into the interactive session under SYSTEM. Probes
  // Winlogon access and writes desktop_probe_child.txt (avoid clobbering the
  // service's log).
  if (argc > 1 && std::strcmp(argv[1], "--probe-desktop") == 0) {
    {
      wchar_t mod[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, mod, MAX_PATH);
      wchar_t* slash = wcsrchr(mod, L'\\');
      if (slash) {
        *slash = L'\0';
      }
      std::wstring path = std::wstring(mod) + L"\\desktop_probe_child.txt";
      g_log = _wfopen(path.c_str(), L"w");
    }
    RunProbeChild();
    if (g_log) {
      std::fclose(g_log);
    }
    return 0;
  }

  const bool as_service = argc > 1 && std::strcmp(argv[1], "--service") == 0;
  const bool as_inject = argc > 2 && std::strcmp(argv[1], "--inject") == 0;
  const DWORD inject_session = as_inject ? (DWORD)std::atoi(argv[2]) : 0;
  // Mirror output to desktop_probe.txt beside the exe (readable when run from
  // task scheduler / SYSTEM non-interactively).
  {
    wchar_t mod[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, mod, MAX_PATH);
    wchar_t* slash = wcsrchr(mod, L'\\');
    if (slash) {
      *slash = L'\0';
    }
    std::wstring path = std::wstring(mod) + L"\\desktop_probe.txt";
    g_log = _wfopen(path.c_str(), L"w");
  }

  if (as_service) {
    SERVICE_TABLE_ENTRYW table[] = {
        {L"desktop_probe", SvcMain},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(table)) {
      // Not started by SCM (e.g. double-click) — run the probe body directly.
      SvcMain(0, nullptr);
    }
    if (g_log) {
      std::fclose(g_log);
    }
    return 0;
  }

  if (as_inject) {
    Out("=== desktop_probe (inject mode) ===\n");
    Out("injecting into session %lu ...\n", inject_session);
    InjectIntoSession(inject_session);
    Out("=== inject mode done ===\n");
    if (g_log) {
      std::fclose(g_log);
    }
    return 0;
  }

  Out("=== desktop_probe ===\n");
  Out("process elevated admin=%d\n", RunningAsAdmin() ? 1 : 0);

  // 1) Current process integrity level (High/Medium/Low/SYSTEM).
  {
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
      BYTE buf[64] = {};
      DWORD got = 0;
      if (GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof(buf), &got)) {
        DWORD sid_attr = *reinterpret_cast<DWORD*>(buf);
        PSID sid = reinterpret_cast<PSID>(buf + sizeof(DWORD));
        char name[64] = {};
        char domain[64] = {};
        DWORD n = sizeof(name);
        DWORD d = sizeof(domain);
        SID_NAME_USE use = SidTypeUnknown;
        if (LookupAccountSidA(nullptr, sid, name, &n, domain, &d, &use)) {
          Out("integrity level: %s\\%s\n", domain, name);
        }
      }
      CloseHandle(tok);
    }
  }

  // 2) Active console session id.
  const DWORD csid = WTSGetActiveConsoleSessionId();
  Out("active console session: %lu\n", csid);

  // 3) Can we open the "Default" desktop (user desktop)?
  {
    HDESK h = OpenDesktopW(L"Default", 0, FALSE,
                           DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP);
    Step("OpenDesktop(Default)", h != nullptr, GetLastError());
    if (h) {
      CloseDesktop(h);
    }
  }

  // 4) Can we open the "Winlogon" desktop (secure / lock screen)?
  {
    HDESK h = OpenDesktopW(L"Winlogon", 0, FALSE,
                           DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP);
    Step("OpenDesktop(Winlogon)", h != nullptr, GetLastError());
    if (h) {
      CloseDesktop(h);
    }
  }

  // 5) OpenInputDesktop — what the secure desktop shows right now.
  {
    HDESK h = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS);
    Step("OpenInputDesktop", h != nullptr, GetLastError());
    if (h) {
      wchar_t name[256] = {};
      DWORD need = 0;
      if (GetUserObjectInformationW(h, UOI_NAME, name, sizeof(name), &need)) {
          Out("      input desktop name: %ls\n", name);
      }
      CloseDesktop(h);
    }
  }

  // 6) If Winlogon opens, try to get a DC for it and BitBlt (screen capture test).
  {
    HDESK h = OpenDesktopW(L"Winlogon", 0, FALSE,
                           DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP);
    if (h) {
      HDC dc = GetDC(nullptr);
      if (dc) {
        const int w = GetSystemMetrics(SM_CXSCREEN);
        const int hh = GetSystemMetrics(SM_CYSCREEN);
        HDC mem = CreateCompatibleDC(dc);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -hh;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, dib);
        const BOOL blt = BitBlt(mem, 0, 0, w, hh, dc, 0, 0, SRCCOPY);
        Step("BitBlt(Winlogon dc)", blt, GetLastError());
        Out("      screen=%dx%d bits=%p\n", w, hh, bits);
        // Sample a few pixels to see if there is real content (not all-zero).
        uint32_t nonzero = 0;
        if (blt && bits) {
          const uint32_t* px = static_cast<const uint32_t*>(bits);
          const size_t n = static_cast<size_t>(w) * static_cast<size_t>(hh);
          const size_t stride = n > 65536 ? n / 65536 : 1;
          for (size_t i = 0; i < n; i += stride) {
            if (px[i] != 0) {
              ++nonzero;
            }
          }
          Out("      sampled non-black pixels: %u / ~%u\n",
                      static_cast<unsigned>(nonzero), static_cast<unsigned>(n / stride));
        }
        SelectObject(mem, old);
        DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, dc);
      } else {
        Step("GetDC for Winlogon", FALSE, GetLastError());
      }
      CloseDesktop(h);
    }
  }

  Out("=== desktop_probe done ===\n");
  if (g_log) {
    std::fclose(g_log);
  }
  return 0;
}
