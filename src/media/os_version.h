#pragma once

// Host OS version for capture strategy (docs/host-os-capture-strategy.md).
// Split: NT < 6.2 = legacy (Mirror/GDI); NT >= 6.2 = modern (DXGI/GDI).
// Server 2008 is 6.0 → legacy. Do not use "is Win7?" as the gate.

namespace road_desk::replace {

struct OsVersionInfo {
  unsigned major = 0;
  unsigned minor = 0;
  unsigned build = 0;
  bool ok = false;
};

enum class CaptureStrategy {
  Legacy,  // Server 2008 / Win7 / 2008 R2
  Modern,  // Server 2012 / Win8+
};

// ntdll!RtlGetVersion. On failure: ok=false and zeros (caller treats as legacy).
OsVersionInfo query_os_version();

// Failure / unknown → true (never open DXGI on Server 2008 by mistake).
bool is_legacy_capture_host(const OsVersionInfo& v);

CaptureStrategy capture_strategy_for(const OsVersionInfo& v);
const char* capture_strategy_name(CaptureStrategy s);

}  // namespace road_desk::replace
