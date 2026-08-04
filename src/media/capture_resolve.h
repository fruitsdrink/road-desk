#pragma once

#include "os_version.h"

namespace road_desk::replace {

enum class CaptureMode {
  Auto,    // legacy: Mirror→GDI; modern: GDI until DXGI (see resolve_capture)
  Mirror,  // require Mirror attach (env override; may fail on modern)
  Gdi,     // force GDI + CPU dirty
  Dxgi,    // modern DXGI Desktop Duplication → GDI on failure
};

// Env ROAD_DESK_CAPTURE + OS strategy → SessionCapture mode.
struct CaptureResolve {
  OsVersionInfo os;
  CaptureStrategy strategy = CaptureStrategy::Legacy;
  CaptureMode requested = CaptureMode::Auto;
  CaptureMode effective = CaptureMode::Auto;
  const char* note = "";
};

CaptureMode parse_capture_mode(const char* s);
const char* capture_mode_name(CaptureMode m);

// Apply docs/host-os-capture-strategy.md (injectable OS for tests).
CaptureResolve resolve_capture_for(const OsVersionInfo& os, const char* env_value);
CaptureResolve resolve_capture(const char* env_value);

}  // namespace road_desk::replace
