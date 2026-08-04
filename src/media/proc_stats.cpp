#include "proc_stats.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <atomic>

namespace road_desk::media {

ProcStats sample_proc_stats() {
  ProcStats st;

  static std::atomic<unsigned long long> last_kernel_100ns{0};
  static std::atomic<unsigned long long> last_user_100ns{0};
  static std::atomic<unsigned long long> last_ticks_ms{0};

  FILETIME create_ft{};
  FILETIME exit_ft{};
  FILETIME kernel_ft{};
  FILETIME user_ft{};
  if (GetProcessTimes(GetCurrentProcess(), &create_ft, &exit_ft, &kernel_ft, &user_ft)) {
    ULARGE_INTEGER kernel{};
    kernel.HighPart = kernel_ft.dwHighDateTime;
    kernel.LowPart = kernel_ft.dwLowDateTime;
    ULARGE_INTEGER user{};
    user.HighPart = user_ft.dwHighDateTime;
    user.LowPart = user_ft.dwLowDateTime;

    const unsigned long long now_ms = GetTickCount64();
    const unsigned long long last_ms = last_ticks_ms.load();
    if (last_ms != 0 && now_ms > last_ms) {
      const unsigned long long d_cpu_100ns =
          (kernel.QuadPart + user.QuadPart) -
          (last_kernel_100ns.load() + last_user_100ns.load());
      const unsigned long long d_wall_ms = now_ms - last_ms;
      // CPU time is in 100ns units; wall is in ms (1 ms = 10000 * 100ns).
      st.cpu_one_core_pct =
          (static_cast<double>(d_cpu_100ns) / (static_cast<double>(d_wall_ms) * 10000.0)) * 100.0;
    }
    last_kernel_100ns.store(kernel.QuadPart);
    last_user_100ns.store(user.QuadPart);
    last_ticks_ms.store(now_ms);
  }

  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  const DWORD nproc = si.dwNumberOfProcessors > 0 ? si.dwNumberOfProcessors : 1u;
  st.cpu_machine_pct = st.cpu_one_core_pct / static_cast<double>(nproc);

  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof(pmc);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    st.ws_bytes = pmc.WorkingSetSize;
  }

  MEMORYSTATUSEX msx{};
  msx.dwLength = sizeof(msx);
  if (GlobalMemoryStatusEx(&msx) && msx.ullTotalPhys > 0) {
    st.mem_pct = (static_cast<double>(st.ws_bytes) / static_cast<double>(msx.ullTotalPhys)) * 100.0;
  }

  return st;
}

}  // namespace road_desk::media
