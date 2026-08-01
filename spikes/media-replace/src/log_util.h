#pragma once

// File + stderr logging. Each log_open() truncates the file (overwrite per run).

#include <cstdarg>

namespace road_desk::replace {

// Opens "<exe_dir>/<name>" for write (truncate). Also mirrors to stderr.
// Returns false if file open failed (stderr-only still works).
bool log_open(const char* filename);

void log_close();

// When false, logf writes only the file (if open). Host disables console mirror
// during a session so tick spam does not dirty-capture its own console window.
void log_set_mirror_stderr(bool on);

// Timestamped line: "YYYY-MM-DD HH:MM:SS.mmm [tag] ..."
void logf(const char* tag, const char* fmt, ...);

const char* log_path();

}  // namespace road_desk::replace
