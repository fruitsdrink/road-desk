#pragma once

// Private media-replace framing (Track P). Not RFB.
// Frame: u8 channel_id | u32 le payload_len | payload

#include <cstdint>

namespace road_desk::replace {

enum Channel : uint8_t {
  kChannelControl = 1,
  kChannelInput = 2,
  kChannelVideo = 3,
  kChannelCursor = 4,
};

enum ControlType : uint8_t {
  kCtrlAuth = 1,      // client -> host: u16 le pass_len + utf8
  kCtrlAuthOk = 2,    // host -> client: u16 le width, u16 le height
  kCtrlAuthFail = 3,  // host -> client: u16 le reason_len + utf8
  kCtrlPing = 4,
  kCtrlPong = 5,
};

enum VideoCodec : uint8_t {
  kVideoRawBgra = 1,   // top-down BGRA8; payload after header is w*h*4 bytes
  kVideoZlibBgra = 2,  // zlib of the same raw BGRA; uncompressed size = w*h*4
  // CopyRect: header x,y,w,h = dst; then u16 le src_x, src_y (no pixels).
  kVideoCopyRect = 3,
};

enum CursorMsg : uint8_t {
  // type | flags | hot_x | hot_y | w | h | optional BGRA
  kCursorShape = 1,
};

constexpr uint8_t kCursorFlagHidden = 1;
constexpr size_t kCursorShapeHeaderSize = 1 + 1 + 2 + 2 + 2 + 2;

enum InputType : uint8_t {
  kInputPointer = 1,  // u8 buttons | u16 le x | u16 le y  (VNC-like: 1=L 2=M 4=R)
  // u8 flags | u16 le vk — flags: bit0=down, bit1=extended (KF_EXTENDED / numpad Enter)
  kInputKey = 2,
};

constexpr uint8_t kInputKeyFlagDown = 1;
constexpr uint8_t kInputKeyFlagExtended = 2;

// Video payload: u8 codec | u32 le frame_id | u16 le x,y,w,h | pixels
constexpr size_t kVideoHeaderSize = 1 + 4 + 2 * 4;
constexpr size_t kCopyRectPayloadSize = kVideoHeaderSize + 2 + 2;
constexpr size_t kInputPointerSize = 1 + 1 + 2 + 2;
constexpr size_t kInputKeySize = 1 + 1 + 2;

// Hard cap — malformed len must disconnect (pitfall E5).
constexpr uint32_t kMaxPayloadLen = 16u * 1024u * 1024u;
constexpr uint32_t kMaxControlPayload = 64u * 1024u;

}  // namespace road_desk::replace
