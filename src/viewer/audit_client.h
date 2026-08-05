#pragma once

#include "gateway_config.h"

#include <string>

namespace road_desk::viewer {

// Process-wide directory credentials for audit upsert (JWT is session-only).
void audit_set_directory(const DirectoryConfig& cfg);
DirectoryConfig audit_directory_copy();
bool audit_reporting_enabled();

std::string audit_new_session_id();

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
};

// Fire-and-forget POST /v1/audit/sessions/upsert. Never blocks the UI thread long;
// failures only go to viewer.log. No-op when audit_reporting_enabled() is false.
void audit_report_async(const AuditReport& report);

}  // namespace road_desk::viewer
