#pragma once

// Auto session recording under <exe>/debug/.
// Prefer MF MP4 (H.264/MJPG); on failure (typical Win7) fall back to VFW MJPEG AVI.
// Only the latest session video + log snapshot are retained.

#include <cstdint>
#include <string>

namespace road_desk::media {

struct SessionRecorderInfo {
  std::string video_path;
  std::string log_copy_path;
  std::string manifest_path;
  bool active = false;
};

// Returns false if disabled or both MF and AVI fallback failed.
// On hard failure, further begin() calls no-op until session_recorder_reset().
bool session_recorder_begin(int width, int height, SessionRecorderInfo* info);
void session_recorder_push_bgra(const uint8_t* bgra, int width, int height);
void session_recorder_end();

void session_recorder_reset();

bool session_recorder_active();
std::string session_recorder_debug_dir();

// Recording is currently hard-disabled (code retained, not deleted):
// session_recorder_enabled() always returns false and the
// ROAD_DESK_AUTO_RECORD env override no longer has any effect.
bool session_recorder_enabled();

}  // namespace road_desk::media
