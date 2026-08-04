#pragma once

// Best-effort process CPU / memory sampling for diagnostic log lines.
// Safe on Win7 x64: GetProcessTimes + GetTickCount64 + GlobalMemoryStatusEx
// + GetProcessMemoryInfo (psapi).

namespace road_desk::media {

struct ProcStats {
  // Process CPU since the previous sample, as % of ONE logical core
  // (100 = one full core; encode/capture here is effectively single-threaded).
  double cpu_one_core_pct = 0.0;
  // Same delta as % of ALL logical cores (Task-Manager-like; = cpu_one_core/nproc).
  double cpu_machine_pct = 0.0;
  // Working set of this process, bytes.
  unsigned long long ws_bytes = 0;
  // Working set as % of total physical RAM.
  double mem_pct = 0.0;
};

// First call returns cpu=0 (no prior sample); memory is valid from the first call.
ProcStats sample_proc_stats();

}  // namespace road_desk::media
