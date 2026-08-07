#include "audit_client.h"

#include "common/http_helpers.h"
#include "log.h"
#include "process_audit.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace road_desk::agent {
namespace {

std::mutex g_cfg_mu;
GatewayConfig g_cfg;
std::atomic<bool> g_cfg_set{false};

std::string build_json(const AuditReport& r, const GatewayConfig& cfg) {
  std::string j = "{";
  auto add_str = [&](const char* key, const std::string& v, bool* first) {
    if (v.empty()) {
      return;
    }
    if (!*first) {
      j += ',';
    }
    *first = false;
    j += '"';
    j += key;
    j += "\":\"";
    j += road_desk::common::json_escape(v);
    j += '"';
  };
  auto add_bool = [&](const char* key, bool v, bool* first) {
    if (!*first) {
      j += ',';
    }
    *first = false;
    j += '"';
    j += key;
    j += "\":";
    j += v ? "true" : "false";
  };
  bool first = true;
  add_str("id", r.session_id, &first);
  add_str("source", "agent", &first);
  add_str("phase", r.phase, &first);
  add_str("viewerIp", r.viewer_ip, &first);
  add_str("agentId", cfg.agent_id, &first);
  add_str("agentName", road_desk::common::local_computer_name(), &first);
  add_str("result", r.result, &first);
  add_str("disconnectReason", r.disconnect_reason, &first);
  add_str("mode", r.mode, &first);
  add_bool("partial", r.partial, &first);
  j += '}';
  return j;
}

std::string build_events_json(const std::vector<AuditBehaviorEvent>& events) {
  std::string j = "{\"events\":[";
  for (size_t i = 0; i < events.size(); ++i) {
    const AuditBehaviorEvent& e = events[i];
    if (i > 0) {
      j += ',';
    }
    j += "{\"sessionId\":\"";
    j += road_desk::common::json_escape(e.session_id);
    j += "\",\"source\":\"agent\",\"type\":\"";
    j += road_desk::common::json_escape(e.type);
    j += '"';
    if (!e.at_iso.empty()) {
      j += ",\"at\":\"";
      j += road_desk::common::json_escape(e.at_iso);
      j += '"';
    }
    j += ",\"detail\":";
    if (!e.detail_json.empty() && e.detail_json.front() == '{') {
      j += e.detail_json;
    } else {
      j += "{}";
    }
    j += '}';
  }
  j += "]}";
  return j;
}

}  // namespace

// Re-export from common for the header's public API compatibility.
std::string audit_json_escape(const std::string& s) {
  return road_desk::common::json_escape(s);
}

void audit_set_agent(const GatewayConfig& cfg) {
  std::lock_guard<std::mutex> lock(g_cfg_mu);
  g_cfg = cfg;
  g_cfg_set.store(!g_cfg.gateway_url.empty() && !g_cfg.agent_psk.empty() && !g_cfg.agent_id.empty());
}

bool audit_reporting_enabled() {
  return g_cfg_set.load();
}

std::string audit_new_session_id() {
  return road_desk::common::new_session_id();
}

void audit_report_async(const AuditReport& report) {
  if (!audit_reporting_enabled()) {
    return;
  }
  GatewayConfig cfg;
  {
    std::lock_guard<std::mutex> lock(g_cfg_mu);
    cfg = g_cfg;
  }
  if (cfg.gateway_url.empty() || cfg.agent_psk.empty() || cfg.agent_id.empty()) {
    return;
  }
  AuditReport copy = report;
  if (copy.session_id.empty()) {
    copy.session_id = audit_new_session_id();
  }
  std::string url = cfg.gateway_url;
  std::string key = cfg.agent_psk;
  std::thread([url, key, cfg, copy]() {
    const std::string body = build_json(copy, cfg);
    std::string err;
    if (!road_desk::common::http_post_json(url, "/v1/audit/sessions/upsert", key, body, 0, &err)) {
      char line[256];
      std::snprintf(line, sizeof(line), "audit upsert fail phase=%s id=%s err=%s",
                    copy.phase.c_str(), copy.session_id.c_str(), err.c_str());
      log_line(line);
    } else {
      char line[192];
      std::snprintf(line, sizeof(line), "audit upsert ok phase=%s id=%s", copy.phase.c_str(),
                    copy.session_id.c_str());
      log_line(line);
    }
  }).detach();
}

void audit_events_report_async(const std::vector<AuditBehaviorEvent>& events) {
  if (!audit_reporting_enabled() || events.empty()) {
    return;
  }
  GatewayConfig cfg;
  {
    std::lock_guard<std::mutex> lock(g_cfg_mu);
    cfg = g_cfg;
  }
  if (cfg.gateway_url.empty() || cfg.agent_psk.empty()) {
    return;
  }

  // Cap at gateway limit (200); split into chunks if needed.
  constexpr size_t kMax = 200;
  size_t off = 0;
  while (off < events.size()) {
    const size_t n = (events.size() - off > kMax) ? kMax : (events.size() - off);
    std::vector<AuditBehaviorEvent> chunk(events.begin() + static_cast<std::ptrdiff_t>(off),
                                          events.begin() + static_cast<std::ptrdiff_t>(off + n));
    off += n;
    std::string url = cfg.gateway_url;
    std::string key = cfg.agent_psk;
    std::thread([url, key, chunk]() {
      const std::string body = build_events_json(chunk);
      std::string err;
      if (!road_desk::common::http_post_json(url, "/v1/audit/events", key, body, 0, &err)) {
        char line[192];
        std::snprintf(line, sizeof(line), "audit events fail n=%u err=%s",
                      static_cast<unsigned>(chunk.size()), err.c_str());
        log_line(line);
        // One retry after short delay (session upsert may still be in flight).
        Sleep(1500);
        if (!road_desk::common::http_post_json(url, "/v1/audit/events", key, body, 0, &err)) {
          std::snprintf(line, sizeof(line), "audit events retry fail n=%u err=%s",
                        static_cast<unsigned>(chunk.size()), err.c_str());
          log_line(line);
        } else {
          std::snprintf(line, sizeof(line), "audit events ok n=%u (retry)",
                        static_cast<unsigned>(chunk.size()));
          log_line(line);
        }
      } else {
        char line[128];
        std::snprintf(line, sizeof(line), "audit events ok n=%u",
                      static_cast<unsigned>(chunk.size()));
        log_line(line);
      }
    }).detach();
  }
}

void audit_on_media_event(void* /*user*/, const media::MediaPlaneConfig::AuditEvent* ev) {
  if (!ev || !ev->phase) {
    return;
  }
  AuditReport r;
  if (ev->session_id && ev->session_id[0]) {
    r.session_id = ev->session_id;
  }
  r.phase = ev->phase;
  if (ev->result) {
    r.result = ev->result;
  }
  if (ev->disconnect_reason) {
    r.disconnect_reason = ev->disconnect_reason;
  }
  if (ev->viewer_ip) {
    r.viewer_ip = ev->viewer_ip;
  }
  if (ev->mode && ev->mode[0]) {
    r.mode = ev->mode;
  }
  // Same Viewer UUID → merge; Host is authoritative for peer/auth/capacity/mode.
  // Capacity reject often has no Viewer id → keep partial until only-host row.
  const bool has_viewer_sid = !r.session_id.empty();
  if (has_viewer_sid && (r.phase == "opened" || r.phase == "closed" || r.phase == "failed")) {
    r.partial = false;
  } else {
    r.partial = true;
  }
  audit_report_async(r);

  // A5b: start/stop process timeline for the active control session.
  process_audit_on_control_session(r.session_id, r.phase.c_str(),
                                   r.mode.empty() ? nullptr : r.mode.c_str());
}

}  // namespace road_desk::agent
