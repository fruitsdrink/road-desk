#pragma once

// Single file log next to the exe (viewer.log / host-agent.log).
// media_log_open() truncates unless the same file is already open.

#include <cstdarg>

namespace road_desk::media {

// Opens "<exe_dir>/<name>" for write (truncate). No-op success if already open
// to the same path. Also mirrors to stderr.
// Returns false if file open failed (stderr-only still works).
bool media_log_open(const char* filename);

bool media_log_is_open();

void media_log_close();

// When false, media_logf writes only the file (if open). Host disables console
// mirror during a session so tick spam does not dirty-capture its own console.
void media_log_set_mirror_stderr(bool on);

// Timestamped line: "YYYY-MM-DD HH:MM:SS.mmm [tag] ..."
void media_logf(const char* tag, const char* fmt, ...);

const char* media_log_path();

}  // namespace road_desk::media
