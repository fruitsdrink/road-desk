#pragma once

#include "gateway_config.h"

#include <string>
#include <vector>

namespace road_desk::viewer {

enum class BookNodeKind { kGroup, kDevice };

struct BookNode {
  int id = 0;
  int parent_id = -1;
  BookNodeKind kind = BookNodeKind::kGroup;
  std::wstring name;
  std::wstring role;    // list: online status (在线/离线)
  std::wstring remark;  // list: free-form note (not host:port)
  std::wstring computer_role;  // admin computerRole: 收费/发卡/…
  std::wstring install_location;  // admin installLocation: 入口01车道/…
  std::wstring lane_number;       // admin laneNumber (optional)
  std::string host;     // preferred IPv4
  int port = 38471;
  std::string version;
  std::string agent_id;   // gateway agentId (empty for demo)
  std::string hostname;   // OS computer name from heartbeat
  std::string ipv4s;      // comma-separated
  std::string last_seen;  // RFC3339 from gateway
  std::string tags;       // comma-separated tag names
  std::string inventory_json;  // Host inventory object JSON
  bool online = false;
};

// Human-readable detail text for the console right pane (UTF-16).
std::wstring address_book_format_detail(const BookNode* device);

enum class AddressBookSource { kDemo, kGateway };

// Load demo tree or fetch gateway directory. Returns false on gateway failure.
bool address_book_load(AddressBookSource source, const DirectoryConfig& dir, std::string* err);

AddressBookSource address_book_source();
const std::vector<BookNode>& address_book_nodes();
std::vector<const BookNode*> address_book_children(int parent_id);
std::vector<const BookNode*> address_book_devices_under(int group_id);
const BookNode* address_book_find(int id);

}  // namespace road_desk::viewer
