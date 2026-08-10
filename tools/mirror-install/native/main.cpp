// Road Desk Mirror — single-file native Win7 x64 installer.
// Signed drivers/cert are embedded as RCDATA (built on the WDK/dev PC).
// This EXE never runs SignTool; it only installs pre-signed payloads.

#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <newdev.h>
#include <shlobj.h>

#include "resource.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr wchar_t kTitle[] = L"Road Desk Mirror";
constexpr wchar_t kHwIdGm2[] = L"Root\\RoadDesk_XPDM_Mirror1";
constexpr GUID kGuidDisplay = {
    0x4D36E968, 0xE325, 0x11CE, {0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18}};

std::wstring g_pkg;
bool g_uninstall_only = false;
bool g_no_reboot_prompt = false;

void Log(const wchar_t* fmt, ...) {
  wchar_t buf[2048];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
  va_end(ap);
  fputws(buf, stdout);
  fputws(L"\n", stdout);
  fflush(stdout);
}

void LogA(const char* s) {
  fputs(s, stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

void ErrorBox(const wchar_t* text);
void InfoBox(const wchar_t* text);
int AskYesNo(const wchar_t* text);

std::wstring ExeDir() {
  wchar_t path[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return L".";
  }
  wchar_t* slash = wcsrchr(path, L'\\');
  if (!slash) {
    return L".";
  }
  *slash = L'\0';
  return path;
}

std::wstring Join(const std::wstring& a, const wchar_t* b) {
  if (a.empty()) {
    return b;
  }
  if (a.back() == L'\\' || a.back() == L'/') {
    return a + b;
  }
  return a + L"\\" + b;
}

bool FileExists(const std::wstring& p) {
  const DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool EnsureDir(const std::wstring& dir) {
  if (CreateDirectoryW(dir.c_str(), nullptr)) {
    return true;
  }
  return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool ExtractResourceToFile(int id, const std::wstring& path) {
  const HRSRC hrsrc = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
  if (!hrsrc) {
    return false;
  }
  const HGLOBAL hglob = LoadResource(nullptr, hrsrc);
  if (!hglob) {
    return false;
  }
  const void* data = LockResource(hglob);
  const DWORD size = SizeofResource(nullptr, hrsrc);
  if (!data || size == 0) {
    return false;
  }
  const HANDLE file =
      CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(file, data, size, &written, nullptr);
  CloseHandle(file);
  return ok && written == size;
}

// Prefer loose files beside the EXE (dev). Otherwise extract embedded RCDATA payload
// so the customer only needs RoadDeskMirrorSetup.exe.
bool PreparePackageDir() {
  const std::wstring exe_dir = ExeDir();
  if (FileExists(Join(exe_dir, L"rdmmirror.sys")) &&
      FileExists(Join(exe_dir, L"rdmmini.sys")) &&
      FileExists(Join(exe_dir, L"rdmdisp.dll")) &&
      FileExists(Join(exe_dir, L"rdm_xpdm.inf")) &&
      FileExists(Join(exe_dir, L"RoadDeskTestCert.cer"))) {
    g_pkg = exe_dir;
    Log(L"Using payload files next to EXE: %s", g_pkg.c_str());
    return true;
  }

  wchar_t progdata[MAX_PATH] = {};
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, progdata))) {
    if (GetTempPathW(MAX_PATH, progdata) == 0) {
      ErrorBox(L"无法定位 ProgramData/Temp 以释放驱动载荷。");
      return false;
    }
  }

  const std::wstring base = Join(progdata, L"RoadDesk");
  g_pkg = Join(base, L"mirror-setup-payload");
  if (!EnsureDir(base) || !EnsureDir(g_pkg)) {
    ErrorBox(L"无法创建载荷目录。");
    return false;
  }

  Log(L"Extracting embedded payload to:");
  Log(L"  %s", g_pkg.c_str());

  struct Item {
    int id;
    const wchar_t* name;
  };
  const Item items[] = {
      {IDR_RDMMIRROR_SYS, L"rdmmirror.sys"},
      {IDR_RDMMINI_SYS, L"rdmmini.sys"},
      {IDR_RDMDISP_DLL, L"rdmdisp.dll"},
      {IDR_RDM_XPDM_INF, L"rdm_xpdm.inf"},
      {IDR_TEST_CERT, L"RoadDeskTestCert.cer"},
  };
  for (const Item& it : items) {
    const std::wstring path = Join(g_pkg, it.name);
    if (!ExtractResourceToFile(it.id, path)) {
      Log(L"Failed to extract %s (FindResource/Write err=%lu)", it.name, GetLastError());
      ErrorBox(L"安装程序未内嵌驱动载荷，或释放失败。\n"
               L"请在开发机重新运行 make-package.ps1 生成完整的 RoadDeskMirrorSetup.exe。");
      return false;
    }
    Log(L"  ok %s", it.name);
  }
  return true;
}

bool IsAdmin() {
  BOOL is_member = FALSE;
  PSID admin_sid = nullptr;
  SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
  if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0,
                                0, 0, 0, 0, &admin_sid)) {
    return false;
  }
  CheckTokenMembership(nullptr, admin_sid, &is_member);
  FreeSid(admin_sid);
  return is_member == TRUE;
}

int AskYesNo(const wchar_t* text) {
  return MessageBoxW(nullptr, text, kTitle, MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);
}

void InfoBox(const wchar_t* text) {
  MessageBoxW(nullptr, text, kTitle, MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
}

void ErrorBox(const wchar_t* text) {
  MessageBoxW(nullptr, text, kTitle, MB_OK | MB_ICONERROR | MB_TOPMOST);
}

// Run command; returns process exit code (-1 on CreateProcess failure).
int RunHidden(const std::wstring& cmdline, DWORD timeout_ms = 60000) {
  std::wstring mutable_cmd = cmdline;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, &mutable_cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      g_pkg.empty() ? nullptr : g_pkg.c_str(), &si, &pi)) {
    Log(L"CreateProcess failed (%lu): %s", GetLastError(), cmdline.c_str());
    return -1;
  }
  const DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
  if (wait == WAIT_TIMEOUT) {
    Log(L"TIMEOUT (%lums): %s", timeout_ms, cmdline.c_str());
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 3000);
  }
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return static_cast<int>(code);
}

std::string RunCapture(const std::wstring& cmdline, DWORD timeout_ms = 30000) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE rd = nullptr;
  HANDLE wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) {
    return {};
  }
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

  std::wstring mutable_cmd = cmdline;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.hStdOutput = wr;
  si.hStdError = wr;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, &mutable_cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                      g_pkg.empty() ? nullptr : g_pkg.c_str(), &si, &pi)) {
    CloseHandle(rd);
    CloseHandle(wr);
    return {};
  }
  CloseHandle(wr);

  std::string out;
  char buf[512];
  DWORD got = 0;
  for (;;) {
    if (!ReadFile(rd, buf, sizeof(buf), &got, nullptr) || got == 0) {
      break;
    }
    out.append(buf, buf + got);
  }
  WaitForSingleObject(pi.hProcess, timeout_ms);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(rd);
  return out;
}

bool TestSigningOn() {
  const std::string o = RunCapture(L"bcdedit");
  const size_t p = o.find("testsigning");
  if (p == std::string::npos) {
    return false;
  }
  const size_t yes = o.find("Yes", p);
  const size_t no = o.find("No", p);
  if (yes == std::string::npos) {
    return false;
  }
  if (no != std::string::npos && no < yes) {
    return false;
  }
  return true;
}

bool ServiceExists(const wchar_t* name) {
  const std::wstring cmd = std::wstring(L"sc.exe query ") + name;
  const std::string o = RunCapture(cmd);
  // English STATE or common Chinese "状态"; exclude "does not exist" / 1060.
  if (o.find("1060") != std::string::npos) {
    return false;
  }
  return o.find("STATE") != std::string::npos || o.find("状态") != std::string::npos;
}

bool ServiceRunning(const wchar_t* name) {
  const std::wstring cmd = std::wstring(L"sc.exe query ") + name;
  const std::string o = RunCapture(cmd);
  return o.find("RUNNING") != std::string::npos || o.find("正在运行") != std::string::npos;
}

bool MirrorInstalled() {
  if (ServiceExists(L"rdmmirror") || ServiceExists(L"rdmmini")) {
    return true;
  }
  wchar_t windir[MAX_PATH] = {};
  GetWindowsDirectoryW(windir, MAX_PATH);
  const std::wstring root = windir;
  if (FileExists(Join(root, L"system32\\drivers\\rdmmirror.sys")) ||
      FileExists(Join(root, L"system32\\drivers\\rdmmini.sys"))) {
    return true;
  }
  // Driver packages registered with pnputil may remain even if services/files
  // were removed. Detect any Road Desk oem##.inf package so install cleans it.
  const std::string enum_out = RunCapture(L"pnputil -e");
  return enum_out.find("Road Desk") != std::string::npos ||
         enum_out.find("rdm_xpdm") != std::string::npos ||
         enum_out.find("RoadDesk") != std::string::npos;
}

bool EnableTestSigning() {
  Log(L"Enabling OS testsigning mode (bcdedit) — not signing drivers on this PC...");
  if (RunHidden(L"bcdedit /set testsigning on") != 0) {
    return false;
  }
  RunHidden(L"bcdedit /set nointegritychecks on");
  return true;
}

bool ImportCert() {
  const std::wstring cer = Join(g_pkg, L"RoadDeskTestCert.cer");
  if (!FileExists(cer)) {
    ErrorBox(L"缺少 RoadDeskTestCert.cer（应在开发机 make-package 签名后打入包内）。");
    return false;
  }
  Log(L"Importing test certificate...");
  std::wstring c1 = L"certutil -addstore Root \"" + cer + L"\"";
  std::wstring c2 = L"certutil -addstore TrustedPublisher \"" + cer + L"\"";
  if (RunHidden(c1) != 0 || RunHidden(c2) != 0) {
    ErrorBox(L"导入证书失败（certutil）。");
    return false;
  }
  return true;
}

// pnputil -e lists each installed driver package. Multiple installs leave several
// oem##.inf copies of the same Road Desk package. Delete in a loop until none remain
// (a package disappears from the enum after its delete, so re-enumerate each pass).
void RemoveRoadDeskPackages() {
  for (int pass = 0; pass < 10; ++pass) {
    const std::string enum_out = RunCapture(L"pnputil -e");
    bool any = false;
    std::string oem;
    std::string line;
    for (size_t i = 0; i <= enum_out.size(); ++i) {
      const char ch = (i < enum_out.size()) ? enum_out[i] : '\n';
      if (ch == '\n' || ch == '\r') {
        if (!line.empty()) {
          // oem##.inf on the same or adjacent line.
          const size_t p = line.find("oem");
          if (p != std::string::npos) {
            const size_t e = line.find(".inf", p);
            if (e != std::string::npos && e > p) {
              oem = line.substr(p, e + 4 - p);
            }
          }
          const bool is_rd =
              line.find("Road Desk") != std::string::npos || line.find("rdm_xpdm") != std::string::npos ||
              line.find("roaddesk") != std::string::npos || line.find("RoadDesk") != std::string::npos;
          if (!oem.empty() && is_rd) {
            LogA(("  pnputil -f -d " + oem).c_str());
            RunHidden(L"pnputil -f -d " + std::wstring(oem.begin(), oem.end()));
            oem.clear();
            any = true;
          }
          line.clear();
        }
      } else {
        line.push_back(ch);
      }
    }
    if (!any) {
      break;
    }
    Sleep(200);
  }
}

void UninstallFast() {
  Log(L"=== Uninstall existing Road Desk Mirror ===");

  const std::wstring probe = Join(g_pkg, L"mirror_probe.exe");
  if (FileExists(probe)) {
    Log(L"[1/4] mirror_probe detach (max 5s)...");
    std::wstring cmd = L"\"" + probe + L"\" detach";
    RunHidden(cmd, 5000);
  } else {
    Log(L"[1/4] no mirror_probe.exe — skip detach");
  }

  Log(L"[2/4] stop/delete services...");
  for (const wchar_t* svc : {L"rdmmirror", L"rdmmini"}) {
    if (!ServiceExists(svc)) {
      Log(L"  %s not registered", svc);
      continue;
    }
    Log(L"  sc stop %s", svc);
    const std::wstring stop = std::wstring(L"sc.exe stop ") + svc;
    const std::string so = RunCapture(stop);
    if (so.find("1052") != std::string::npos) {
      Log(L"  (ignore 1052: service does not accept stop)");
    }
    Sleep(300);
    Log(L"  sc delete %s", svc);
    RunHidden(std::wstring(L"sc.exe delete ") + svc);
  }

  Log(L"[3/4] pnputil remove Road Desk packages (loop until none left)...");
  RemoveRoadDeskPackages();

  Log(L"[4/4] remove driver files if not locked...");
  wchar_t windir[MAX_PATH] = {};
  GetWindowsDirectoryW(windir, MAX_PATH);
  const std::wstring root = windir;
  for (const wchar_t* rel :
       {L"system32\\drivers\\rdmmirror.sys", L"system32\\drivers\\rdmmini.sys", L"system32\\rdmdisp.dll"}) {
    const std::wstring path = Join(root, rel);
    if (FileExists(path)) {
      if (DeleteFileW(path.c_str())) {
        Log(L"  removed %s", path.c_str());
      } else {
        Log(L"  leave %s (in use; install will overwrite)", path.c_str());
      }
    }
  }
  Log(L"Uninstall pass done.");
}

bool InstallGm1() {
  Log(L"Installing GM1 (rdmmirror)...");
  const std::wstring src = Join(g_pkg, L"rdmmirror.sys");
  if (!FileExists(src)) {
    ErrorBox(L"缺少 rdmmirror.sys");
    return false;
  }
  wchar_t windir[MAX_PATH] = {};
  GetWindowsDirectoryW(windir, MAX_PATH);
  const std::wstring dst = Join(windir, L"system32\\drivers\\rdmmirror.sys");
  if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
    Log(L"CopyFile rdmmirror.sys failed (%lu)", GetLastError());
    return false;
  }

  if (!ServiceExists(L"rdmmirror")) {
    // sc.exe requires spaces after '=' tokens.
    const int rc = RunHidden(
        L"sc.exe create rdmmirror type= kernel start= demand error= normal "
        L"binPath= \\SystemRoot\\system32\\drivers\\rdmmirror.sys "
        L"DisplayName= \"Road Desk Mirror\"");
    if (rc != 0 && !ServiceExists(L"rdmmirror")) {
      ErrorBox(L"sc create rdmmirror 失败");
      return false;
    }
  }

  if (!ServiceRunning(L"rdmmirror")) {
    const std::string o = RunCapture(L"sc.exe start rdmmirror");
    LogA(o.c_str());
    if (o.find("577") != std::string::npos) {
      ErrorBox(L"sc start 错误 577：系统测试模式或证书未就绪。\n"
               L"请确认已重启（开启测试模式后），并使用开发机已签名的驱动包。");
      return false;
    }
    if (!ServiceRunning(L"rdmmirror") && o.find("1056") == std::string::npos &&
        o.find("already") == std::string::npos) {
      // 1056 = already running (English); Chinese variants may differ — re-query.
      if (!ServiceRunning(L"rdmmirror")) {
        ErrorBox(L"sc start rdmmirror 失败");
        return false;
      }
    }
  }
  Log(L"GM1 OK");
  return true;
}

bool CreateRootDeviceAndUpdate(const std::wstring& hw_id, const std::wstring& inf_path) {
  GUID class_guid = kGuidDisplay;

  // Remove any existing Road Desk mirror device nodes first. Repeated installs
  // without removal leave multiple device instances; the device manager can then
  // surface an old instance bound to a stale INF (old DriverVer) even though the
  // binaries and INF in the driver store are new.
  {
    Log(L"Removing existing Road Desk mirror device node(s)...");
    HDEVINFO devs = SetupDiGetClassDevsW(&class_guid, nullptr, nullptr, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) {
      devs = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    }
    if (devs != INVALID_HANDLE_VALUE) {
      for (DWORD idx = 0;; ++idx) {
        SP_DEVINFO_DATA di{};
        di.cbSize = sizeof(di);
        if (!SetupDiEnumDeviceInfo(devs, idx, &di)) {
          break;
        }
        wchar_t hwids[2048] = {};
        DWORD need = 0;
        if (SetupDiGetDeviceRegistryPropertyW(devs, &di, SPDRP_HARDWAREID, nullptr,
                                              reinterpret_cast<BYTE*>(hwids), sizeof(hwids),
                                              &need) &&
            wcsstr(hwids, hw_id.c_str())) {
          Log(L"  removing device %ls ...", di.DevInst);
          SetupDiCallClassInstaller(DIF_REMOVE, devs, &di);
        }
      }
      SetupDiDestroyDeviceInfoList(devs);
    }
  }

  HDEVINFO list = SetupDiCreateDeviceInfoList(&class_guid, nullptr);
  if (list == INVALID_HANDLE_VALUE) {
    Log(L"SetupDiCreateDeviceInfoList failed (%lu)", GetLastError());
    return false;
  }

  SP_DEVINFO_DATA data{};
  data.cbSize = sizeof(data);
  BOOL ok = SetupDiCreateDeviceInfoW(list, L"RoadDeskMirror", &class_guid, L"Road Desk Mirror Driver",
                                     nullptr, DICD_GENERATE_ID, &data);
  if (!ok) {
    Log(L"SetupDiCreateDeviceInfo failed (%lu)", GetLastError());
    SetupDiDestroyDeviceInfoList(list);
    return false;
  }

  // MULTI_SZ hardware ID
  std::vector<wchar_t> hid;
  hid.insert(hid.end(), hw_id.begin(), hw_id.end());
  hid.push_back(L'\0');
  hid.push_back(L'\0');
  ok = SetupDiSetDeviceRegistryPropertyW(list, &data, SPDRP_HARDWAREID,
                                         reinterpret_cast<const BYTE*>(hid.data()),
                                         static_cast<DWORD>(hid.size() * sizeof(wchar_t)));
  if (!ok) {
    Log(L"SetupDiSetDeviceRegistryProperty failed (%lu)", GetLastError());
    SetupDiDestroyDeviceInfoList(list);
    return false;
  }

  ok = SetupDiCallClassInstaller(DIF_REGISTERDEVICE, list, &data);
  if (!ok) {
    Log(L"DIF_REGISTERDEVICE failed (%lu) — may already exist", GetLastError());
  }
  SetupDiDestroyDeviceInfoList(list);

  BOOL reboot = FALSE;
  ok = UpdateDriverForPlugAndPlayDevicesW(nullptr, hw_id.c_str(), inf_path.c_str(), INSTALLFLAG_FORCE,
                                          &reboot);
  if (!ok) {
    Log(L"UpdateDriverForPlugAndPlayDevices failed (%lu) — continuing if already present",
        GetLastError());
  } else {
    Log(L"UpdateDriver OK (rebootRequired=%d)", reboot ? 1 : 0);
  }
  return true;
}

bool InstallGm2() {
  Log(L"Installing GM2 (rdmmini / rdmdisp)...");
  const std::wstring mini = Join(g_pkg, L"rdmmini.sys");
  const std::wstring disp = Join(g_pkg, L"rdmdisp.dll");
  const std::wstring inf = Join(g_pkg, L"rdm_xpdm.inf");
  if (!FileExists(mini) || !FileExists(disp) || !FileExists(inf)) {
    ErrorBox(L"缺少 rdmmini.sys / rdmdisp.dll / rdm_xpdm.inf");
    return false;
  }
  wchar_t windir[MAX_PATH] = {};
  GetWindowsDirectoryW(windir, MAX_PATH);
  if (!CopyFileW(mini.c_str(), Join(windir, L"system32\\drivers\\rdmmini.sys").c_str(), FALSE) ||
      !CopyFileW(disp.c_str(), Join(windir, L"system32\\rdmdisp.dll").c_str(), FALSE)) {
    Log(L"CopyFile GM2 failed (%lu)", GetLastError());
    return false;
  }

  // Remove any pre-existing Road Desk driver package (oem##.inf) so the INF
  // registered below is the ONLY one. Leftover old packages make the device
  // manager report the old DriverVer even though the binaries were overwritten.
  Log(L"Removing pre-existing Road Desk driver packages before pnputil add...");
  RemoveRoadDeskPackages();

  Log(L"pnputil -i -a rdm_xpdm.inf ...");
  RunHidden(L"pnputil -i -a \"" + inf + L"\"");

  Log(L"Creating root device %s ...", kHwIdGm2);
  CreateRootDeviceAndUpdate(kHwIdGm2, inf);
  Log(L"GM2 staged (reboot required for display enum).");
  return true;
}

void OfferReboot(const wchar_t* msg) {
  if (g_no_reboot_prompt) {
    return;
  }
  if (AskYesNo(msg) == IDYES) {
    // /t 3 short delay, no /c (the /c comment pops a second "Close" confirmation
    // dialog that can look like the reboot silently failed).
    RunHidden(L"shutdown.exe /r /t 3");
  }
}

void ParseArgs(int argc, wchar_t** argv) {
  for (int i = 1; i < argc; ++i) {
    if (_wcsicmp(argv[i], L"/uninstall") == 0 || _wcsicmp(argv[i], L"-uninstall") == 0) {
      g_uninstall_only = true;
    } else if (_wcsicmp(argv[i], L"/noreboot") == 0 || _wcsicmp(argv[i], L"-noreboot") == 0) {
      g_no_reboot_prompt = true;
    }
  }
}

int RunInstall() {
  Log(L"Package: %s", g_pkg.c_str());
  Log(L"Drivers must already be SignTool-signed on the build PC.");
  Log(L"");

  if (g_uninstall_only) {
    UninstallFast();
    InfoBox(L"Road Desk Mirror 已卸载。若文件仍被占用，请重启后再清理残留。");
    return 0;
  }

  for (const wchar_t* f :
       {L"rdmmirror.sys", L"rdmmini.sys", L"rdmdisp.dll", L"rdm_xpdm.inf", L"RoadDeskTestCert.cer"}) {
    if (!FileExists(Join(g_pkg, f))) {
      std::wstring msg = L"安装包不完整，缺少: ";
      msg += f;
      msg += L"\n请使用开发机 make-package.ps1 生成完整 win7-x64 目录。";
      ErrorBox(msg.c_str());
      return 1;
    }
  }

  if (!TestSigningOn()) {
    if (!EnableTestSigning()) {
      ErrorBox(L"无法打开系统测试模式（bcdedit）。请确认以管理员运行。");
      return 1;
    }
    OfferReboot(L"已打开系统「测试模式」(bcdedit testsigning)。\n\n"
                L"这不是在本机签名驱动——驱动应已在开发机签好。\n\n"
                L"必须先重启，重启后请再次运行 RoadDeskMirrorSetup.exe 完成安装。\n\n"
                L"是否立即重启？");
    return 0;
  }

  if (!ImportCert()) {
    return 1;
  }

  // Always uninstall first — repeated installs without cleanup leave multiple Road
  // Desk mirror drivers (services / oem##.inf copies) that break attach and capture.
  if (MirrorInstalled()) {
    Log(L"Existing install detected — uninstalling first...");
  } else {
    Log(L"No install detected — cleaning residual packages anyway...");
  }
  UninstallFast();
  Sleep(500);

  if (!InstallGm1()) {
    return 1;
  }
  if (!InstallGm2()) {
    return 1;
  }

  Log(L"");
  Log(L"Install finished. Reboot is required.");
  OfferReboot(L"Road Desk Mirror 已安装（使用开发机已签名的驱动）。\n\n"
              L"必须重启后才能正常使用。\n"
              L"Host 请以管理员运行 host-agent.exe。\n\n"
              L"是否立即重启？");
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  ParseArgs(argc, argv);

  if (!IsAdmin()) {
    ErrorBox(L"需要管理员权限。请右键「以管理员身份运行」，或同意 UAC 提示。");
    return 1;
  }

  // Keep a console when double-clicked from Explorer.
  if (GetConsoleWindow() == nullptr) {
    AllocConsole();
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
  }

  SetConsoleTitleW(kTitle);
  Log(L"Road Desk Mirror Setup (single-file)");
  Log(L"");

  if (!PreparePackageDir()) {
    Log(L"");
    Log(L"Failed to prepare payload. Press Enter to close...");
    (void)getchar();
    return 1;
  }

  const int rc = RunInstall();
  if (rc != 0) {
    Log(L"");
    Log(L"Failed (exit %d). Press Enter to close...", rc);
    (void)getchar();
  } else if (!g_no_reboot_prompt) {
    // Brief pause so console users can read the last lines if they declined reboot.
    Log(L"");
    Log(L"Done. Press Enter to close...");
    (void)getchar();
  }
  return rc;
}
