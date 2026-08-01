#pragma once

#include <string>
#include <vector>

namespace road_desk::viewer {

enum class DemoNodeKind { kGroup, kDevice };

struct DemoNode {
  int id = 0;
  int parent_id = -1;  // -1 = root
  DemoNodeKind kind = DemoNodeKind::kGroup;
  std::wstring name;
  std::wstring role;    // list column
  std::wstring remark;  // e.g. "演示：同 Host"
};

// Built-in highway demo tree (唐山东/唐港/唐山南/田庄 → 机房/车道).
const std::vector<DemoNode>& demo_book_nodes();

// Children of parent_id (parent_id=-1 for top-level under synthetic root).
std::vector<const DemoNode*> demo_book_children(int parent_id);

// Device nodes under group (recursive).
std::vector<const DemoNode*> demo_book_devices_under(int group_id);

const DemoNode* demo_book_find(int id);

}  // namespace road_desk::viewer
