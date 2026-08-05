#pragma once

#include "gateway_config.h"
#include "media_plane.h"

#include <string>

namespace road_desk::agent {

// Process-wide gateway credentials for Host audit upsert (Agent-PSK).
void audit_set_agent(const GatewayConfig& cfg);
bool audit_reporting_enabled();

std::string audit_new_session_id();

struct AuditReport {
  std::string session_id;
  std::string phase;  // opened|closed|failed
  std::string viewer_ip;
  std::string result;  // ok|auth_fail|capacity_reject|tls_fail|...
  std::string disconnect_reason;
  std::string mode;  // control|view_only (A5a)
  bool partial = true;
};

// Fire-and-forget POST /v1/audit/sessions/upsert. Never blocks the media path long.
void audit_report_async(const AuditReport& report);

// MediaPlaneConfig::AuditFn — maps mux host events to async upsert.
void audit_on_media_event(void* user, const media::MediaPlaneConfig::AuditEvent* ev);

}  // namespace road_desk::agent
