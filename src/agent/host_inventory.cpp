#include "host_inventory.h"

#include "media/os_version.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace road_desk::agent {
namespace {

std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        o += "\\\"";
        break;
      case '\\':
        o += "\\\\";
        break;
      case '\n':
        o += "\\n";
        break;
      case '\r':
        o += "\\r";
        break;
      case '\t':
        o += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          o += buf;
        } else {
          o.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return o;
}

std::string wide_to_utf8(const wchar_t* w) {
  if (!w || !w[0]) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) {
    return {};
  }
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::string os_friendly_name(const road_desk::replace::OsVersionInfo& v) {
  if (!v.ok) {
    return "Windows";
  }
  // Coarse labels for directory display (not a full SKU map).
  if (v.major == 6 && v.minor == 1) {
    return "Windows 7 / Server 2008 R2";
  }
  if (v.major == 6 && v.minor == 0) {
    return "Windows Vista / Server 2008";
  }
  if (v.major == 6 && v.minor == 2) {
    return "Windows 8 / Server 2012";
  }
  if (v.major == 6 && v.minor == 3) {
    return "Windows 8.1 / Server 2012 R2";
  }
  if (v.major == 10) {
    if (v.build >= 22000) {
      return "Windows 11 / Server 2022+";
    }
    return "Windows 10 / Server 2016+";
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "Windows %u.%u", v.major, v.minor);
  return buf;
}

std::string query_cpu_name() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ,
                    &key) != ERROR_SUCCESS) {
    return {};
  }
  wchar_t name[512] = {};
  DWORD typ = 0;
  DWORD cb = sizeof(name);
  const LONG rc =
      RegQueryValueExW(key, L"ProcessorNameString", nullptr, &typ, reinterpret_cast<LPBYTE>(name),
                       &cb);
  RegCloseKey(key);
  if (rc != ERROR_SUCCESS || (typ != REG_SZ && typ != REG_EXPAND_SZ)) {
    return {};
  }
  // Trim leading spaces common in the registry value.
  const wchar_t* p = name;
  while (*p == L' ' || *p == L'\t') {
    ++p;
  }
  return wide_to_utf8(p);
}

std::string drive_type_name(UINT t) {
  switch (t) {
    case DRIVE_FIXED:
      return "fixed";
    case DRIVE_REMOVABLE:
      return "removable";
    case DRIVE_REMOTE:
      return "network";
    case DRIVE_CDROM:
      return "cdrom";
    case DRIVE_RAMDISK:
      return "ramdisk";
    default:
      return "unknown";
  }
}

ULARGE_INTEGER filetime_to_ularge(const FILETIME& ft) {
  ULARGE_INTEGER u{};
  u.HighPart = ft.dwHighDateTime;
  u.LowPart = ft.dwLowDateTime;
  return u;
}

bool read_system_times(unsigned long long* idle, unsigned long long* kernel, unsigned long long* user) {
  FILETIME idle_ft{}, kernel_ft{}, user_ft{};
  if (!GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) {
    return false;
  }
  *idle = filetime_to_ularge(idle_ft).QuadPart;
  *kernel = filetime_to_ularge(kernel_ft).QuadPart;
  *user = filetime_to_ularge(user_ft).QuadPart;
  return true;
}

bool cpu_busy_percent(unsigned long long i0, unsigned long long k0, unsigned long long u0,
                      unsigned long long i1, unsigned long long k1, unsigned long long u1,
                      double* out_pct) {
  const unsigned long long d_idle = i1 - i0;
  const unsigned long long d_total = (k1 - k0) + (u1 - u0);
  if (d_total == 0 || !out_pct) {
    return false;
  }
  // Kernel includes idle; busy = total - idle.
  const unsigned long long d_busy = d_total > d_idle ? d_total - d_idle : 0;
  double pct = (static_cast<double>(d_busy) * 100.0) / static_cast<double>(d_total);
  if (pct < 0.0) {
    pct = 0.0;
  }
  if (pct > 100.0) {
    pct = 100.0;
  }
  *out_pct = pct;
  return true;
}

// System-wide CPU busy % since last sample (150ms probe on first call).
bool sample_cpu_usage_percent(double* out_pct) {
  static bool have_prev = false;
  static unsigned long long prev_idle = 0;
  static unsigned long long prev_kernel = 0;
  static unsigned long long prev_user = 0;

  unsigned long long idle_t = 0;
  unsigned long long kernel_t = 0;
  unsigned long long user_t = 0;
  if (!read_system_times(&idle_t, &kernel_t, &user_t)) {
    return false;
  }

  if (!have_prev) {
    Sleep(150);
    unsigned long long idle2 = 0;
    unsigned long long kernel2 = 0;
    unsigned long long user2 = 0;
    if (!read_system_times(&idle2, &kernel2, &user2)) {
      return false;
    }
    const bool ok = cpu_busy_percent(idle_t, kernel_t, user_t, idle2, kernel2, user2, out_pct);
    prev_idle = idle2;
    prev_kernel = kernel2;
    prev_user = user2;
    have_prev = true;
    return ok;
  }

  const bool ok =
      cpu_busy_percent(prev_idle, prev_kernel, prev_user, idle_t, kernel_t, user_t, out_pct);
  prev_idle = idle_t;
  prev_kernel = kernel_t;
  prev_user = user_t;
  return ok;
}

}  // namespace

std::string collect_host_inventory_json() {
  const auto osv = road_desk::replace::query_os_version();
  char ver_buf[64] = {};
  if (osv.ok) {
    std::snprintf(ver_buf, sizeof(ver_buf), "%u.%u.%u", osv.major, osv.minor, osv.build);
  }

  wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1] = {};
  DWORD cn = MAX_COMPUTERNAME_LENGTH + 1;
  GetComputerNameW(computer, &cn);

  SYSTEM_INFO si{};
  GetNativeSystemInfo(&si);
  const char* arch = "unknown";
  switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
      arch = "x64";
      break;
    case PROCESSOR_ARCHITECTURE_INTEL:
      arch = "x86";
      break;
    case PROCESSOR_ARCHITECTURE_ARM64:
      arch = "arm64";
      break;
    default:
      break;
  }
  const DWORD cpu_count = si.dwNumberOfProcessors > 0 ? si.dwNumberOfProcessors : 1u;

  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  unsigned long long mem_total_mb = 0;
  unsigned long long mem_avail_mb = 0;
  unsigned long long mem_used_mb = 0;
  unsigned mem_load_pct = 0;
  if (GlobalMemoryStatusEx(&mem)) {
    mem_total_mb = mem.ullTotalPhys / (1024ull * 1024ull);
    mem_avail_mb = mem.ullAvailPhys / (1024ull * 1024ull);
    if (mem.ullTotalPhys >= mem.ullAvailPhys) {
      mem_used_mb = (mem.ullTotalPhys - mem.ullAvailPhys) / (1024ull * 1024ull);
    }
    mem_load_pct = static_cast<unsigned>(mem.dwMemoryLoad);
  }

  double cpu_pct = 0.0;
  const bool have_cpu = sample_cpu_usage_percent(&cpu_pct);
  char cpu_pct_s[32] = {};
  if (have_cpu) {
    std::snprintf(cpu_pct_s, sizeof(cpu_pct_s), "%.1f", cpu_pct);
  }

  const unsigned long long uptime_sec = GetTickCount64() / 1000ull;

  const std::string cpu = query_cpu_name();
  const std::string os_name = os_friendly_name(osv);
  const std::string computer_u8 = wide_to_utf8(computer);

  std::ostringstream ss;
  ss << '{'
     << "\"os\":\"" << json_escape(os_name) << "\","
     << "\"osVersion\":\"" << json_escape(ver_buf) << "\","
     << "\"arch\":\"" << arch << "\","
     << "\"computerName\":\"" << json_escape(computer_u8) << "\","
     << "\"cpu\":\"" << json_escape(cpu) << "\","
     << "\"cpuCount\":" << cpu_count << ','
     << "\"memoryTotalMb\":" << mem_total_mb << ','
     << "\"memoryUsedMb\":" << mem_used_mb << ','
     << "\"memoryAvailMb\":" << mem_avail_mb << ','
     << "\"memoryLoadPercent\":" << mem_load_pct << ','
     << "\"uptimeSec\":" << uptime_sec << ',';
  if (have_cpu) {
    ss << "\"cpuUsagePercent\":" << cpu_pct_s << ',';
  }
  ss << "\"disks\":[";

  wchar_t drives[512] = {};
  const DWORD dn = GetLogicalDriveStringsW(static_cast<DWORD>((sizeof(drives) / sizeof(drives[0])) - 1),
                                          drives);
  bool first_disk = true;
  if (dn > 0 && dn < (sizeof(drives) / sizeof(drives[0]))) {
    for (wchar_t* p = drives; *p; p += wcslen(p) + 1) {
      const UINT dt = GetDriveTypeW(p);
      if (dt != DRIVE_FIXED && dt != DRIVE_REMOVABLE && dt != DRIVE_RAMDISK) {
        continue;  // skip network/cdrom noise for directory panel
      }
      ULARGE_INTEGER free_bytes{}, total_bytes{}, total_free{};
      if (!GetDiskFreeSpaceExW(p, &free_bytes, &total_bytes, &total_free)) {
        continue;
      }
      wchar_t vol[MAX_PATH] = {};
      wchar_t fs[64] = {};
      GetVolumeInformationW(p, vol, MAX_PATH, nullptr, nullptr, nullptr, fs, 64);
      const double total_gb = static_cast<double>(total_bytes.QuadPart) / (1024.0 * 1024.0 * 1024.0);
      const double free_gb = static_cast<double>(free_bytes.QuadPart) / (1024.0 * 1024.0 * 1024.0);
      char total_s[32];
      char free_s[32];
      std::snprintf(total_s, sizeof(total_s), "%.1f", total_gb);
      std::snprintf(free_s, sizeof(free_s), "%.1f", free_gb);
      if (!first_disk) {
        ss << ',';
      }
      first_disk = false;
      ss << '{'
         << "\"letter\":\"" << json_escape(wide_to_utf8(p)) << "\","
         << "\"label\":\"" << json_escape(wide_to_utf8(vol)) << "\","
         << "\"fs\":\"" << json_escape(wide_to_utf8(fs)) << "\","
         << "\"type\":\"" << drive_type_name(dt) << "\","
         << "\"totalGb\":" << total_s << ','
         << "\"freeGb\":" << free_s << '}';
    }
  }
  ss << "]}";
  return ss.str();
}

}  // namespace road_desk::agent
