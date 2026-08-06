#pragma once

#include "gateway_config.h"
#include "media_plane.h"

#include <string>
#include <vector>

namespace road_desk::agent {

// Process-wide gateway credentials for Host audit upsert (Agent-PSK).
void audit_set_agent(const GatewayConfig& cfg);
bool audit_reporting_enabled();

std::string audit_new_session_id();

// Escape a string for embedding in a JSON string value.
std::string audit_json_escape(const std::string& s);

struct AuditReport {
  std::string session_id;
  std::string phase;  // opened|closed|failed|flag
  std::string viewer_ip;
  std::string result;  // ok|auth_fail|capacity_reject|tls_fail|...
  std::string disconnect_reason;
  std::string mode;  // control|view_only (A5a)
  bool partial = true;
};

// Fire-and-forget POST /v1/audit/sessions/upsert. Never blocks the media path long.
void audit_report_async(const AuditReport& report);

// A5b behavior event for POST /v1/audit/events (detail_json is a JSON object string).
struct AuditBehaviorEvent {
  std::string session_id;
  std::string type;        // process_open | process_close | ...
  std::string at_iso;      // optional RFC3339; empty → gateway now
  std::string detail_json; // object, e.g. {"name":"…","pid":123}
};

// Fire-and-forget batch POST /v1/audit/events (≤200). Never blocks the media path long.
void audit_events_report_async(const std::vector<AuditBehaviorEvent>& events);

// MediaPlaneConfig::AuditFn — maps mux host events to async upsert (+ A5b monitor).
void audit_on_media_event(void* user, const media::MediaPlaneConfig::AuditEvent* ev);

}  // namespace road_desk::agent
