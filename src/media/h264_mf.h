#pragma once

// Media Foundation H.264 for modern Host (Win8+). Rects encoded as independent IDR.

#include "video_encode.h"

#include <cstdint>
#include <vector>

namespace road_desk::media {

// quality: 1..100 (maps to CODECAPI_AVEncCommonQuality). Even w/h required (auto-aligned down).
bool encode_h264_rect(uint32_t frame_id, int desk_w, int desk_h, int x, int y, int rw, int rh,
                      const uint8_t* bgra_full, int quality, std::vector<uint8_t>* scratch_nv12,
                      std::vector<uint8_t>* body_buf, EncodeRectStats* st);

bool decode_h264_to_bgra(const uint8_t* annex_b, size_t len, int expect_w, int expect_h,
                         std::vector<uint8_t>* bgra_out);

void h264_codec_shutdown();

}  // namespace road_desk::media
