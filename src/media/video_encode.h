#pragma once

#include <cstdint>
#include <vector>

namespace road_desk::media {

struct EncodeRectStats {
  uint32_t raw_bytes = 0;
  uint32_t wire_bytes = 0;
  uint8_t codec = 0;
};

// Build Video channel body (header + zlib or raw). No I/O.
// body_buf holds the full mux payload body (not including mux frame header).
bool encode_video_rect(uint32_t frame_id, int desk_w, int desk_h, int x, int y, int rw, int rh,
                       const uint8_t* bgra_full, std::vector<uint8_t>* raw_buf,
                       std::vector<uint8_t>* body_buf, EncodeRectStats* st);

}  // namespace road_desk::media
