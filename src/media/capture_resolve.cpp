#include "capture_resolve.h"

#include <cstring>

namespace road_desk::replace {

CaptureMode parse_capture_mode(const char* s) {
  if (!s || !s[0]) {
    return CaptureMode::Auto;
  }
  if (_stricmp(s, "gdi") == 0) {
    return CaptureMode::Gdi;
  }
  if (_stricmp(s, "mirror") == 0) {
    return CaptureMode::Mirror;
  }
  if (_stricmp(s, "dxgi") == 0) {
    return CaptureMode::Dxgi;
  }
  return CaptureMode::Auto;
}

const char* capture_mode_name(CaptureMode m) {
  switch (m) {
    case CaptureMode::Gdi:
      return "gdi";
    case CaptureMode::Mirror:
      return "mirror";
    case CaptureMode::Dxgi:
      return "dxgi";
    case CaptureMode::Auto:
    default:
      return "auto";
  }
}

CaptureResolve resolve_capture_for(const OsVersionInfo& os, const char* env_value) {
  CaptureResolve r;
  r.os = os;
  r.strategy = capture_strategy_for(os);
  r.requested = parse_capture_mode(env_value);
  r.effective = r.requested;
  r.note = "";

  if (!r.os.ok) {
    r.note = "os_version_failed; strategy=legacy";
  }

  if (r.requested == CaptureMode::Dxgi) {
    if (r.strategy == CaptureStrategy::Legacy) {
      r.effective = CaptureMode::Gdi;
      r.note = r.os.ok ? "dxgi forbidden on legacy; using gdi"
                       : "os_version_failed; dxgi forbidden; using gdi";
    } else {
      r.effective = CaptureMode::Dxgi;
      r.note = "";
    }
    return r;
  }

  if (r.requested == CaptureMode::Auto && r.strategy == CaptureStrategy::Modern) {
    r.effective = CaptureMode::Dxgi;
    r.note = "modern auto: dxgi (skip mirror)";
  }
  return r;
}

CaptureResolve resolve_capture(const char* env_value) {
  return resolve_capture_for(query_os_version(), env_value);
}

}  // namespace road_desk::replace
