#pragma once

#include <cstdint>
#include <vector>

namespace road_desk::replace {

struct CursorShape {
  bool hidden = true;
  uint16_t hot_x = 0;
  uint16_t hot_y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  std::vector<uint8_t> bgra;  // top-down BGRA, size w*h*4
  uint32_t hash = 0;
};

// Capture current OS cursor as BGRA + hotspot. Returns false on hard failure.
bool capture_cursor_shape(CursorShape* out);

}  // namespace road_desk::replace
