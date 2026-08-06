#pragma once

#include "gateway_config.h"

#include <string>
#include <vector>

namespace road_desk::viewer {

// Process-wide directory credentials for audit upsert (JWT is session-only).
void audit_set_directory(const DirectoryConfig& cfg);
DirectoryConfig audit_directory_copy();
bool audit_reporting_enabled();

std::string audit_new_session_id();

struct AuditFileItem {
  std::string path;  // UTF-8 full path
  std::string name;  // basename
  bool is_dir = false;
  bool outbound = true;  // Viewer→Host
};

struct AuditReport {
  std::string session_id;
  std::string phase;  // attempt|opened|closed|failed|flag
  std::string operator_name;
  std::string viewer_host;
  std::string agent_id;
  std::string agent_name;
  std::string agent_endpoint;
  std::string mode;    // control|view_only|unknown
  std::string result;  // ok|auth_fail|...
  std::string disconnect_reason;
  bool used_clipboard = false;
  bool used_file_transfer = false;
  bool set_clipboard = false;
  bool set_file = false;
  bool partial = true;
  int reconnect_count = -1;  // >=0 → write into meta
  // Cumulative file-transfer stats (Viewer perspective).
  // out = Viewer→Host, in = Host→Viewer. Top-level roots only in file_items.
  int file_out_count = 0;
  int file_in_count = 0;
  int file_out_entries = 0;
  int file_in_entries = 0;
  std::vector<AuditFileItem> file_items;
};

// Fire-and-forget POST /v1/audit/sessions/upsert. Never blocks the UI thread long;
// failures only go to viewer.log. No-op when audit_reporting_enabled() is false.
void audit_report_async(const AuditReport& report);

// Fire-and-forget POST /v1/viewer/heartbeat for admin online Viewer list.
void viewer_presence_heartbeat_async();

// Blocking POST /v1/viewer/offline (short timeout) so graceful exit clears online status.
void viewer_presence_offline_sync();

}  // namespace road_desk::viewer
