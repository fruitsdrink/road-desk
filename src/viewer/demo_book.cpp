#include "demo_book.h"

namespace road_desk::viewer {
namespace {

std::vector<DemoNode> g_nodes;
bool g_ready = false;

int add_group(int parent, const wchar_t* name) {
  DemoNode n;
  n.id = static_cast<int>(g_nodes.size());
  n.parent_id = parent;
  n.kind = DemoNodeKind::kGroup;
  n.name = name;
  g_nodes.push_back(n);
  return n.id;
}

void add_device(int parent, const wchar_t* name, const wchar_t* role) {
  DemoNode n;
  n.id = static_cast<int>(g_nodes.size());
  n.parent_id = parent;
  n.kind = DemoNodeKind::kDevice;
  n.name = name;
  n.role = role;
  n.remark = L"演示：同 Host";
  g_nodes.push_back(n);
}

void fill_station(int station_id) {
  const int room = add_group(station_id, L"机房");
  add_device(room, L"监控主机", L"机房");
  add_device(room, L"收费服务器", L"机房");
  add_device(room, L"特情工作站", L"机房");

  const int lanes = add_group(station_id, L"车道");
  add_device(lanes, L"入口1车道工控机", L"车道");
  add_device(lanes, L"入口2车道工控机", L"车道");
  add_device(lanes, L"出口1车道工控机", L"车道");
  add_device(lanes, L"出口2车道工控机", L"车道");
}

void ensure() {
  if (g_ready) {
    return;
  }
  g_ready = true;
  g_nodes.clear();
  const int root = add_group(-1, L"路段机电");
  fill_station(add_group(root, L"唐山东"));
  fill_station(add_group(root, L"唐港"));
  fill_station(add_group(root, L"唐山南"));
  fill_station(add_group(root, L"田庄"));
}

void collect_devices(int group_id, std::vector<const DemoNode*>* out) {
  for (const auto& n : g_nodes) {
    if (n.parent_id != group_id) {
      continue;
    }
    if (n.kind == DemoNodeKind::kDevice) {
      out->push_back(&n);
    } else {
      collect_devices(n.id, out);
    }
  }
}

}  // namespace

const std::vector<DemoNode>& demo_book_nodes() {
  ensure();
  return g_nodes;
}

std::vector<const DemoNode*> demo_book_children(int parent_id) {
  ensure();
  std::vector<const DemoNode*> out;
  for (const auto& n : g_nodes) {
    if (n.parent_id == parent_id) {
      out.push_back(&n);
    }
  }
  return out;
}

std::vector<const DemoNode*> demo_book_devices_under(int group_id) {
  ensure();
  std::vector<const DemoNode*> out;
  const DemoNode* self = demo_book_find(group_id);
  if (self && self->kind == DemoNodeKind::kDevice) {
    out.push_back(self);
    return out;
  }
  collect_devices(group_id, &out);
  return out;
}

const DemoNode* demo_book_find(int id) {
  ensure();
  if (id < 0 || id >= static_cast<int>(g_nodes.size())) {
    return nullptr;
  }
  return &g_nodes[static_cast<size_t>(id)];
}

}  // namespace road_desk::viewer
