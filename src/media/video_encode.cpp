#include "video_encode.h"

#include "mux_protocol.h"

#include "miniz.h"

#include <cstring>

namespace road_desk::media {
namespace {

using road_desk::replace::kMaxPayloadLen;
using road_desk::replace::kVideoHeaderSize;
using road_desk::replace::kVideoRawBgra;
using road_desk::replace::kVideoZlibBgra;

void write_u16_le(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

void write_u32_le(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

bool rect_in_desk(int desk_w, int desk_h, int x, int y, int rw, int rh) {
  if (desk_w <= 0 || desk_h <= 0 || rw <= 0 || rh <= 0) {
    return false;
  }
  if (x < 0 || y < 0) {
    return false;
  }
  if (x > desk_w - rw || y > desk_h - rh) {
    return false;
  }
  return true;
}

}  // namespace

bool encode_video_rect(uint32_t frame_id, int desk_w, int desk_h, int x, int y, int rw, int rh,
                       const uint8_t* bgra_full, std::vector<uint8_t>* raw_buf,
                       std::vector<uint8_t>* body_buf, EncodeRectStats* st) {
  if (!bgra_full || !raw_buf || !body_buf || !rect_in_desk(desk_w, desk_h, x, y, rw, rh)) {
    return false;
  }
  const uint32_t pix = static_cast<uint32_t>(rw) * static_cast<uint32_t>(rh) * 4u;
  if (pix == 0 || static_cast<uint32_t>(kVideoHeaderSize) + pix > kMaxPayloadLen) {
    return false;
  }
  raw_buf->resize(pix);
  uint8_t* raw = raw_buf->data();
  for (int row = 0; row < rh; ++row) {
    const uint8_t* src = bgra_full + (static_cast<size_t>(y + row) * desk_w + x) * 4u;
    std::memcpy(raw + static_cast<size_t>(row) * rw * 4u, src, static_cast<size_t>(rw) * 4u);
  }

  mz_ulong bound = mz_compressBound(pix);
  body_buf->resize(static_cast<size_t>(kVideoHeaderSize) + static_cast<size_t>(bound));
  uint8_t* body = body_buf->data();
  write_u32_le(body + 1, frame_id);
  write_u16_le(body + 5, static_cast<uint16_t>(x));
  write_u16_le(body + 7, static_cast<uint16_t>(y));
  write_u16_le(body + 9, static_cast<uint16_t>(rw));
  write_u16_le(body + 11, static_cast<uint16_t>(rh));

  mz_ulong zlen = bound;
  const int zrc = mz_compress2(body + kVideoHeaderSize, &zlen, raw, pix, MZ_BEST_SPEED);
  uint32_t total = 0;
  uint8_t codec = kVideoRawBgra;
  if (zrc == MZ_OK && zlen > 0 && zlen < pix) {
    body[0] = kVideoZlibBgra;
    codec = kVideoZlibBgra;
    total = static_cast<uint32_t>(kVideoHeaderSize) + static_cast<uint32_t>(zlen);
  } else {
    body_buf->resize(static_cast<size_t>(kVideoHeaderSize) + pix);
    body = body_buf->data();
    body[0] = kVideoRawBgra;
    write_u32_le(body + 1, frame_id);
    write_u16_le(body + 5, static_cast<uint16_t>(x));
    write_u16_le(body + 7, static_cast<uint16_t>(y));
    write_u16_le(body + 9, static_cast<uint16_t>(rw));
    write_u16_le(body + 11, static_cast<uint16_t>(rh));
    std::memcpy(body + kVideoHeaderSize, raw, pix);
    codec = kVideoRawBgra;
    total = static_cast<uint32_t>(kVideoHeaderSize) + pix;
  }
  body_buf->resize(total);
  if (st) {
    st->raw_bytes = pix;
    st->wire_bytes = total;
    st->codec = codec;
  }
  return true;
}

}  // namespace road_desk::media
