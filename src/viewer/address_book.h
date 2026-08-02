#pragma once

#include <string>
#include <vector>

namespace road_desk::viewer {

enum class BookNodeKind { kGroup, kDevice };

struct BookNode {
  int id = 0;
  int parent_id = -1;
  BookNodeKind kind = BookNodeKind::kGroup;
  std::wstring name;
  std::wstring role;    // list: online / tags
  std::wstring remark;  // list: extra
  std::string host;     // device only
  int port = 38471;
  std::string version;
  bool online = false;
};

enum class AddressBookSource { kDemo, kGateway };

struct DirectoryConfig {
  std::string gateway_url;
  std::string directory_key;
};

DirectoryConfig load_directory_config();

// Load demo tree or fetch gateway directory. Returns false on gateway failure.
bool address_book_load(AddressBookSource source, const DirectoryConfig& dir, std::string* err);

AddressBookSource address_book_source();
const std::vector<BookNode>& address_book_nodes();
std::vector<const BookNode*> address_book_children(int parent_id);
std::vector<const BookNode*> address_book_devices_under(int group_id);
const BookNode* address_book_find(int id);

}  // namespace road_desk::viewer
