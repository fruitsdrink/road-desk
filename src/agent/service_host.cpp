#include "service_host.h"

#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "advapi32.lib")

namespace road_desk::agent {

const wchar_t* kServiceName = L"RoadDeskAgentSrv";
const wchar_t* kServiceDisplayName = L"Road Desk Host Agent";

namespace {

// Global service state and stop event, valid only when running as a service.
SERVICE_STATUS_HANDLE g_svc_handle = nullptr;
SERVICE_STATUS g_svc_status{};
HANDLE g_svc_stop_event = nullptr;

constexpr DWORD kSvcAcceptedControls =
    SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;

void svc_report_status(DWORD current, DWORD exit_code, DWORD wait_hint) {
  g_svc_status.dwCurrentState = current;
  g_svc_status.dwWin32ExitCode = exit_code;
  g_svc_status.dwWaitHint = wait_hint;
  if (current == SERVICE_START_PENDING) {
    g_svc_status.dwControlsAccepted = 0;
  } else {
    g_svc_status.dwControlsAccepted = kSvcAcceptedControls;
  }
  SetServiceStatus(g_svc_handle, &g_svc_status);
}

// This is set by the service main thread once the media plane is ready.
// The control handler can signal stop_event; the main loop polls it.
HANDLE svc_stop_event() {
  return g_svc_stop_event;
}

DWORD WINAPI svc_ctrl_handler(DWORD ctrl, DWORD /*event_type*/, LPVOID /*event_data*/,
                              LPVOID /*context*/) {
  switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      svc_report_status(SERVICE_STOP_PENDING, NO_ERROR, 5000);
      if (g_svc_stop_event) {
        SetEvent(g_svc_stop_event);
      }
      return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE:
      // Handled in S2 via the session notification handler.
      break;
    case SERVICE_CONTROL_INTERROGATE:
      SetServiceStatus(g_svc_handle, &g_svc_status);
      return NO_ERROR;
    default:
      break;
  }
  return NO_ERROR;
}

VOID WINAPI service_main(DWORD /*argc*/, LPWSTR* /*argv*/) {
  g_svc_handle = RegisterServiceCtrlHandlerExW(kServiceName, svc_ctrl_handler, nullptr);
  if (!g_svc_handle) {
    return;
  }

  g_svc_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_svc_status.dwServiceSpecificExitCode = 0;
  svc_report_status(SERVICE_START_PENDING, NO_ERROR, 3000);

  g_svc_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!g_svc_stop_event) {
    svc_report_status(SERVICE_STOPPED, ERROR_OUTOFMEMORY, 0);
    return;
  }

  svc_report_status(SERVICE_RUNNING, NO_ERROR, 0);

  // Hand off to the shared serve loop (defined in main.cpp, in namespace road_desk::agent).
  const int rc = road_desk::agent::host_agent_serve(0, nullptr, /*as_service=*/true);

  svc_report_status(SERVICE_STOPPED, rc != 0 ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR, 0);
  if (g_svc_stop_event) {
    CloseHandle(g_svc_stop_event);
    g_svc_stop_event = nullptr;
  }
}

}  // namespace

bool is_service_cli_arg(const char* arg) {
  return arg &&
         (std::strcmp(arg, "--install") == 0 || std::strcmp(arg, "--uninstall") == 0 ||
          std::strcmp(arg, "--run-as-service") == 0 ||
          (std::strncmp(arg, "--helper", 7) == 0));
}

bool try_run_service_cli(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (!argv[i]) continue;

    if (std::strcmp(argv[i], "--run-as-service") == 0) {
      log_line("service_host: entering ServiceMain (--run-as-service)");
      SERVICE_TABLE_ENTRYW table[] = {
          {const_cast<LPWSTR>(kServiceName), service_main},
          {nullptr, nullptr},
      };
      if (!StartServiceCtrlDispatcherW(table)) {
        char line[128];
        std::snprintf(line, sizeof(line),
                      "service_host: StartServiceCtrlDispatcher failed err=%lu",
                      static_cast<unsigned long>(GetLastError()));
        log_line(line);
        return false;
      }
      return true;
    }

    if (std::strcmp(argv[i], "--install") == 0) {
      char exe_path[MAX_PATH] = {};
      GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
      SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
      if (!scm) {
        char line[128];
        std::snprintf(line, sizeof(line),
                      "service_host: OpenSCManager failed err=%lu (run as Administrator?)",
                      static_cast<unsigned long>(GetLastError()));
        log_line(line);
        std::fprintf(stderr, "%s\n", line);
        return false;
      }
      std::string cmd = std::string("\"") + exe_path + "\" --run-as-service";
      SC_HANDLE svc = CreateServiceW(
          scm, kServiceName, kServiceDisplayName, SERVICE_ALL_ACCESS,
          SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
          std::wstring(cmd.begin(), cmd.end()).c_str(),
          nullptr, nullptr, nullptr, L"LocalSystem", nullptr);
      if (!svc) {
        const DWORD e = GetLastError();
        if (e == ERROR_SERVICE_EXISTS) {
          log_line("service_host: service already installed");
          std::fprintf(stderr, "Service already installed. Use --uninstall first, or sc start %ls\n",
                       kServiceName);
        } else {
          char line[128];
          std::snprintf(line, sizeof(line), "service_host: CreateService failed err=%lu",
                        static_cast<unsigned long>(e));
          log_line(line);
          std::fprintf(stderr, "%s\n", line);
        }
        CloseServiceHandle(scm);
        return false;
      }
      log_line("service_host: service installed (LocalSystem, auto-start)");
      std::fprintf(stderr, "Service installed: %ls (%ls)\n", kServiceDisplayName, kServiceName);
      StartServiceW(svc, 0, nullptr);
      CloseServiceHandle(svc);
      CloseServiceHandle(scm);
      return true;
    }

    if (std::strcmp(argv[i], "--uninstall") == 0) {
      SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
      if (!scm) {
        char line[128];
        std::snprintf(line, sizeof(line),
                      "service_host: OpenSCManager failed err=%lu (run as Administrator?)",
                      static_cast<unsigned long>(GetLastError()));
        log_line(line);
        std::fprintf(stderr, "%s\n", line);
        return false;
      }
      SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
      if (!svc) {
        const DWORD e = GetLastError();
        if (e == ERROR_SERVICE_DOES_NOT_EXIST) {
          std::fprintf(stderr, "Service is not installed.\n");
        } else {
          std::fprintf(stderr, "OpenService failed err=%lu\n", static_cast<unsigned long>(e));
        }
        CloseServiceHandle(scm);
        return false;
      }
      SERVICE_STATUS_PROCESS ssp{};
      DWORD needed = 0;
      if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                               reinterpret_cast<BYTE*>(&ssp), sizeof(ssp), &needed) &&
          ssp.dwCurrentState != SERVICE_STOPPED) {
        ControlService(svc, SERVICE_CONTROL_STOP, reinterpret_cast<SERVICE_STATUS*>(&ssp));
        log_line("service_host: stopping service");
        Sleep(2000);
      }
      if (DeleteService(svc)) {
        log_line("service_host: service uninstalled");
        std::fprintf(stderr, "Service uninstalled.\n");
      } else {
        const DWORD e = GetLastError();
        char line[128];
        std::snprintf(line, sizeof(line), "service_host: DeleteService failed err=%lu",
                      static_cast<unsigned long>(e));
        log_line(line);
        std::fprintf(stderr, "%s\n", line);
      }
      CloseServiceHandle(svc);
      CloseServiceHandle(scm);
      return true;
    }
  }
  return false;
}

int run_as_service(int /*argc*/, char** /*argv*/) {
  SERVICE_TABLE_ENTRYW table[] = {
      {const_cast<LPWSTR>(kServiceName), service_main},
      {nullptr, nullptr},
  };
  if (!StartServiceCtrlDispatcherW(table)) {
    char line[128];
    std::snprintf(line, sizeof(line),
                  "service_host: StartServiceCtrlDispatcher failed err=%lu",
                  static_cast<unsigned long>(GetLastError()));
    log_line(line);
    return 1;
  }
  return 0;
}

}  // namespace road_desk::agent

// Public accessor for the media serve loop (used in main.cpp).
namespace road_desk::agent {
HANDLE service_stop_event() {
  return g_svc_stop_event;
}
}  // namespace road_desk::agent
