#pragma once

// Modern-path lossy rect encode (WIC JPEG). Phase-1 of Win10 "video" track;
// temporal H.264 can replace this codec id later without changing the fan-out path.

#include "video_encode.h"

#include <cstdint>
#include <vector>

namespace road_desk::media {

// quality: 1..100 (typical 70–85). Returns false → caller should fall back to zlib/raw.
bool encode_jpeg_rect(uint32_t frame_id, int desk_w, int desk_h, int x, int y, int rw, int rh,
                      const uint8_t* bgra_full, int quality, std::vector<uint8_t>* raw_buf,
                      std::vector<uint8_t>* body_buf, EncodeRectStats* st);

// Decode JPEG payload (no video header) into top-down BGRA rw*rh*4.
bool decode_jpeg_to_bgra(const uint8_t* jpeg, size_t jpeg_len, int expect_w, int expect_h,
                         std::vector<uint8_t>* bgra_out);

}  // namespace road_desk::media
