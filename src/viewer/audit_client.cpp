#include "audit_client.h"

#include "common/http_helpers.h"
#include "media_log.h"
#include "product_version.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace road_desk::viewer {
namespace {

std::mutex g_dir_mu;
DirectoryConfig g_dir;
std::atomic<bool> g_dir_set{false};

std::string build_json(const AuditReport& r) {
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
  add_str("source", "viewer", &first);
  add_str("phase", r.phase, &first);
  add_str("operatorName", r.operator_name, &first);
  add_str("viewerHost", r.viewer_host.empty() ? road_desk::common::local_computer_name() : r.viewer_host, &first);
  add_str("agentId", r.agent_id, &first);
  add_str("agentName", r.agent_name, &first);
  add_str("agentEndpoint", r.agent_endpoint, &first);
  add_str("mode", r.mode, &first);
  add_str("result", r.result, &first);
  add_str("disconnectReason", r.disconnect_reason, &first);
  if (r.set_clipboard) {
    add_bool("usedClipboard", r.used_clipboard, &first);
  }
  if (r.set_file) {
    add_bool("usedFileTransfer", r.used_file_transfer, &first);
  }
  add_bool("partial", r.partial, &first);

  const bool has_reconnect = r.reconnect_count >= 0;
  const bool has_file_meta = r.file_out_count > 0 || r.file_in_count > 0;
  if (has_reconnect || has_file_meta) {
    if (!first) {
      j += ',';
    }
    first = false;
    j += "\"meta\":{";
    bool meta_first = true;
    if (has_reconnect) {
      char buf[48];
      std::snprintf(buf, sizeof(buf), "\"reconnectCount\":%d", r.reconnect_count);
      j += buf;
      meta_first = false;
    }
    if (has_file_meta) {
      if (!meta_first) {
        j += ',';
      }
      char buf[192];
      std::snprintf(buf, sizeof(buf),
                    "\"fileTransfer\":{\"out\":%s,\"in\":%s,\"outCount\":%d,\"inCount\":%d,"
                    "\"outEntries\":%d,\"inEntries\":%d",
                    r.file_out_count > 0 ? "true" : "false", r.file_in_count > 0 ? "true" : "false",
                    r.file_out_count, r.file_in_count, r.file_out_entries, r.file_in_entries);
      j += buf;
      if (!r.file_items.empty()) {
        j += ",\"items\":[";
        for (size_t i = 0; i < r.file_items.size(); ++i) {
          const AuditFileItem& it = r.file_items[i];
          if (i) {
            j += ',';
          }
          j += "{\"dir\":\"";
          j += it.outbound ? "out" : "in";
          j += "\",\"name\":\"";
          j += road_desk::common::json_escape(it.name);
          j += "\",\"path\":\"";
          j += road_desk::common::json_escape(it.path);
          j += "\",\"isDir\":";
          j += it.is_dir ? "true" : "false";
          j += '}';
        }
        j += ']';
      }
      j += '}';
    }
    j += '}';
  }
  j += '}';
  return j;
}

}  // namespace

void viewer_presence_heartbeat_async();

void audit_set_directory(const DirectoryConfig& cfg) {
  {
    std::lock_guard<std::mutex> lock(g_dir_mu);
    g_dir = cfg;
    if (g_dir.directory_key.empty() && !g_dir.viewer_psk.empty()) {
      g_dir.directory_key = g_dir.viewer_psk;
    }
    ensure_viewer_instance_id(&g_dir);
    g_dir_set.store(!g_dir.gateway_url.empty() && !g_dir.directory_key.empty());
  }
  viewer_presence_heartbeat_async();
}

DirectoryConfig audit_directory_copy() {
  std::lock_guard<std::mutex> lock(g_dir_mu);
  return g_dir;
}

bool audit_reporting_enabled() {
  return g_dir_set.load();
}

std::string audit_new_session_id() {
  return road_desk::common::new_session_id();
}

void audit_report_async(const AuditReport& report) {
  if (!audit_reporting_enabled() || report.session_id.empty()) {
    return;
  }
  DirectoryConfig dir;
  {
    std::lock_guard<std::mutex> lock(g_dir_mu);
    dir = g_dir;
  }
  if (dir.gateway_url.empty() || dir.directory_key.empty()) {
    return;
  }
  AuditReport copy = report;
  if (copy.operator_name.empty() && !dir.username.empty()) {
    copy.operator_name = dir.username;
  }
  if (copy.operator_name.empty() && !dir.viewer_psk.empty() &&
      dir.directory_key == dir.viewer_psk) {
    copy.operator_name = "viewer_psk";
  }
  std::string url = dir.gateway_url;
  std::string key = dir.directory_key;
  std::thread([url, key, copy]() {
    const std::string body = build_json(copy);
    std::string err;
    if (!road_desk::common::http_post_json(url, "/v1/audit/sessions/upsert", key, body, 0, &err)) {
      road_desk::media::media_logf("viewer", "audit upsert fail phase=%s id=%s err=%s",
                                   copy.phase.c_str(), copy.session_id.c_str(), err.c_str());
    } else {
      road_desk::media::media_logf("viewer", "audit upsert ok phase=%s id=%s", copy.phase.c_str(),
                                   copy.session_id.c_str());
    }
  }).detach();
}

void viewer_presence_heartbeat_async() {
  if (!audit_reporting_enabled()) {
    return;
  }
  DirectoryConfig dir;
  {
    std::lock_guard<std::mutex> lock(g_dir_mu);
    dir = g_dir;
  }
  if (dir.gateway_url.empty() || dir.directory_key.empty()) {
    return;
  }
  ensure_viewer_instance_id(&dir);
  {
    std::lock_guard<std::mutex> lock(g_dir_mu);
    g_dir.viewer_instance_id = dir.viewer_instance_id;
  }
  const std::string host = road_desk::common::local_computer_name();
  std::string body = "{\"viewerId\":\"";
  body += road_desk::common::json_escape(dir.viewer_instance_id);
  body += "\",\"hostname\":\"";
  body += road_desk::common::json_escape(host);
  body += "\",\"version\":\"";
  body += road_desk::common::json_escape(ROAD_DESK_VERSION_STRING);
  body += '"';
  if (!dir.username.empty()) {
    body += ",\"username\":\"";
    body += road_desk::common::json_escape(dir.username);
    body += '"';
  }
  body += '}';
  std::string url = dir.gateway_url;
  std::string key = dir.directory_key;
  std::thread([url, key, body]() {
    std::string err;
    if (!road_desk::common::http_post_json(url, "/v1/viewer/heartbeat", key, body, 0, &err)) {
      road_desk::media::media_logf("viewer", "presence heartbeat fail err=%s", err.c_str());
    }
  }).detach();
}

void viewer_presence_offline_sync() {
  if (!audit_reporting_enabled()) {
    return;
  }
  DirectoryConfig dir;
  {
    std::lock_guard<std::mutex> lock(g_dir_mu);
    dir = g_dir;
  }
  if (dir.gateway_url.empty() || dir.directory_key.empty() || dir.viewer_instance_id.empty()) {
    return;
  }
  std::string body = "{\"viewerId\":\"";
  body += road_desk::common::json_escape(dir.viewer_instance_id);
  body += "\"}";
  std::string err;
  if (!road_desk::common::http_post_json(dir.gateway_url, "/v1/viewer/offline",
                                          dir.directory_key, body, 2000, &err)) {
    road_desk::media::media_logf("viewer", "presence offline fail err=%s", err.c_str());
  }
}

}  // namespace road_desk::viewer
