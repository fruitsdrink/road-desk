#include "os_version.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace road_desk::replace {
namespace {

using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);

}  // namespace

OsVersionInfo query_os_version() {
  OsVersionInfo out;
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) {
    return out;
  }
  const auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
  if (!fn) {
    return out;
  }
  OSVERSIONINFOW vi{};
  vi.dwOSVersionInfoSize = sizeof(vi);
  if (fn(&vi) != 0) {
    return out;
  }
  out.major = vi.dwMajorVersion;
  out.minor = vi.dwMinorVersion;
  out.build = vi.dwBuildNumber;
  out.ok = true;
  return out;
}

bool is_legacy_capture_host(const OsVersionInfo& v) {
  if (!v.ok) {
    return true;
  }
  // modern := major > 6 || (major == 6 && minor >= 2)
  if (v.major > 6) {
    return false;
  }
  if (v.major == 6 && v.minor >= 2) {
    return false;
  }
  return true;
}

CaptureStrategy capture_strategy_for(const OsVersionInfo& v) {
  return is_legacy_capture_host(v) ? CaptureStrategy::Legacy : CaptureStrategy::Modern;
}

const char* capture_strategy_name(CaptureStrategy s) {
  return s == CaptureStrategy::Modern ? "modern" : "legacy";
}

}  // namespace road_desk::replace
