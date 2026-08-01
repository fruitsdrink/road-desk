#pragma once

#include "mux_protocol.h"

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace road_desk::replace {

struct ClipboardEchoGuard {
  DWORD ignore_seq = 0;
  uint32_t last_text_digest = 0;
  uint32_t last_bitmap_digest = 0;

  void mark_local_write(uint32_t text_digest);
  void mark_local_bitmap_write(uint32_t bitmap_digest);
  bool should_skip_seq(DWORD seq) const;
};

uint32_t clipboard_text_digest(const uint8_t* utf16, size_t byte_len);
uint32_t clipboard_bytes_digest(const uint8_t* data, size_t byte_len);

// Read CF_UNICODETEXT as UTF-16LE bytes (NUL stripped). False if empty/unavailable/too large.
bool clipboard_read_text_utf16(std::vector<uint8_t>* utf16_out);

// Write CF_UNICODETEXT; updates echo guard with current sequence number.
bool clipboard_write_text_utf16(const uint8_t* utf16, size_t byte_len, ClipboardEchoGuard* guard);

// Read CF_DIB (or convert CF_BITMAP). Output is packed DIB for CF_DIB.
bool clipboard_read_dib(std::vector<uint8_t>* dib_out);
bool clipboard_write_dib(const uint8_t* dib, size_t byte_len, ClipboardEchoGuard* guard);

bool clipboard_has_hdrop();
bool clipboard_has_dib();
bool clipboard_has_text();

// Build / parse kClipText payloads (includes type byte).
bool build_clip_text_payload(const std::vector<uint8_t>& utf16, std::vector<uint8_t>* out);
bool parse_clip_text_payload(const uint8_t* p, size_t n, std::vector<uint8_t>* utf16_out);

// Build / parse kClipBitmap (zlib preferred).
bool build_clip_bitmap_payload(const std::vector<uint8_t>& dib, std::vector<uint8_t>* out);
bool parse_clip_bitmap_payload(const uint8_t* p, size_t n, std::vector<uint8_t>* dib_out);

}  // namespace road_desk::replace
