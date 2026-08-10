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
#include <cwchar>
#include <string>

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

int main() {
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
