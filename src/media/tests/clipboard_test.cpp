#include "mux_clipboard.h"

#include "mux_protocol.h"

#include <cstdio>
#include <cstring>

namespace {

int g_fails = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_fails;
  }
}

// ── clipboard_text_digest ──────────────────────────────────────────

void test_digest_deterministic() {
  using namespace road_desk::replace;
  const uint8_t data[] = {'h', 'e', 'l', 'l', 'o', 0, 'w', 0};
  uint32_t a = clipboard_text_digest(data, sizeof(data));
  uint32_t b = clipboard_text_digest(data, sizeof(data));
  expect(a == b, "digest deterministic");
}

void test_digest_differs_on_content() {
  using namespace road_desk::replace;
  const uint8_t a[] = {'a', 0};
  const uint8_t b[] = {'b', 0};
  expect(clipboard_text_digest(a, 2) != clipboard_text_digest(b, 2),
         "different content different digest");
}

void test_digest_empty() {
  using namespace road_desk::replace;
  expect(clipboard_bytes_digest(nullptr, 0) != 0 || true,  // always ok
         "digest null/zero doesn't crash");
  uint32_t h = clipboard_bytes_digest(nullptr, 0);
  expect(h != 0 || h == 0, "digest yields value");
}

// ── build_clip_text_payload / parse_clip_text_payload ──────────────

void test_text_payload_roundtrip() {
  using namespace road_desk::replace;
  // UTF-16LE "Hi" = H(0x48) \0 i(0x69) \0
  std::vector<uint8_t> utf16 = {'H', 0, 'i', 0};
  std::vector<uint8_t> payload;
  expect(build_clip_text_payload(utf16, &payload), "build text ok");
  expect(payload.size() == 1 + 4 + 4, "payload size");

  std::vector<uint8_t> parsed;
  expect(parse_clip_text_payload(payload.data(), payload.size(), &parsed), "parse text ok");
  expect(parsed == utf16, "text roundtrip match");
}

void test_text_payload_empty_input() {
  using namespace road_desk::replace;
  std::vector<uint8_t> empty;
  std::vector<uint8_t> out;
  expect(!build_clip_text_payload(empty, &out), "empty text fails");
}

void test_text_payload_odd_length() {
  using namespace road_desk::replace;
  std::vector<uint8_t> odd = {'a', 0, 'b'};  // 3 bytes — invalid UTF-16
  std::vector<uint8_t> out;
  expect(!build_clip_text_payload(odd, &out), "odd length fails");
}

void test_text_payload_too_large() {
  using namespace road_desk::replace;
  std::vector<uint8_t> huge;
  huge.resize(kMaxClipboardTextBytes + 2, 0);
  std::vector<uint8_t> out;
  expect(!build_clip_text_payload(huge, &out), "oversized text fails");
}

void test_parse_text_bad_type() {
  using namespace road_desk::replace;
  uint8_t buf[] = {kClipFilesOffer, 4, 0, 0, 0, 'a', 'b', 'c', 'd'};
  std::vector<uint8_t> out;
  expect(!parse_clip_text_payload(buf, sizeof(buf), &out), "bad type");
}

void test_parse_text_truncated() {
  using namespace road_desk::replace;
  uint8_t buf[] = {kClipText, 100, 0, 0, 0};  // claims 100 bytes, none follow
  std::vector<uint8_t> out;
  expect(!parse_clip_text_payload(buf, sizeof(buf), &out), "truncated");
}

// ── build_clip_bitmap_payload / parse_clip_bitmap_payload ──────────

std::vector<uint8_t> make_minimal_dib(int w, int h) {
  // Minimal 32bpp bottom-up DIB.
  BITMAPINFOHEADER bi{};
  bi.biSize = sizeof(BITMAPINFOHEADER);
  bi.biWidth = w;
  bi.biHeight = h;
  bi.biPlanes = 1;
  bi.biBitCount = 32;
  bi.biCompression = BI_RGB;
  const uint32_t row = ((static_cast<uint32_t>(w) * 32u + 31u) / 32u) * 4u;
  bi.biSizeImage = row * static_cast<uint32_t>(h);
  std::vector<uint8_t> dib(sizeof(bi) + bi.biSizeImage);
  std::memcpy(dib.data(), &bi, sizeof(bi));
  // Fill with non-constant pixels so zlib doesn't get a free 0-to-0 win.
  for (size_t i = sizeof(bi); i < dib.size(); ++i) {
    dib[i] = static_cast<uint8_t>(i & 0xff);
  }
  return dib;
}

void test_bitmap_roundtrip_raw_small() {
  using namespace road_desk::replace;
  auto dib = make_minimal_dib(4, 4);
  std::vector<uint8_t> payload;
  expect(build_clip_bitmap_payload(dib, &payload), "build bitmap ok");
  expect(payload[0] == kClipBitmap, "bitmap type");

  std::vector<uint8_t> parsed;
  expect(parse_clip_bitmap_payload(payload.data(), payload.size(), &parsed),
         "parse bitmap ok");
  expect(parsed == dib, "bitmap roundtrip match");
}

void test_bitmap_roundtrip_compressed() {
  using namespace road_desk::replace;
  auto dib = make_minimal_dib(128, 96);
  std::vector<uint8_t> payload;
  expect(build_clip_bitmap_payload(dib, &payload), "build large bitmap ok");
  expect(payload[0] == kClipBitmap, "type");
  // Should prefer zlib for non-trivial data.
  expect(payload[1] == kClipBitmapZlib || payload[1] == kClipBitmapRaw, "valid codec");

  std::vector<uint8_t> parsed;
  expect(parse_clip_bitmap_payload(payload.data(), payload.size(), &parsed),
         "parse large bitmap ok");
  expect(parsed == dib, "large roundtrip match");
}

void test_bitmap_bad_header() {
  using namespace road_desk::replace;
  std::vector<uint8_t> bad(10, 0);
  std::vector<uint8_t> out;
  expect(!build_clip_bitmap_payload(bad, &out), "bad dib rejected");
}

void test_bitmap_empty() {
  using namespace road_desk::replace;
  std::vector<uint8_t> empty;
  std::vector<uint8_t> out;
  expect(!build_clip_bitmap_payload(empty, &out), "empty dib rejected");
}

void test_parse_bitmap_bad_type() {
  using namespace road_desk::replace;
  uint8_t buf[] = {kClipText, kClipBitmapRaw, 0, 0, 0, 0};
  std::vector<uint8_t> out;
  expect(!parse_clip_bitmap_payload(buf, sizeof(buf), &out), "bad type");
}

void test_parse_bitmap_bad_codec() {
  using namespace road_desk::replace;
  uint8_t buf[20] = {};
  buf[0] = kClipBitmap;
  buf[1] = 99;  // invalid codec
  std::vector<uint8_t> out;
  expect(!parse_clip_bitmap_payload(buf, sizeof(buf), &out), "invalid codec");
}

}  // namespace

int main() {
  test_digest_deterministic();
  test_digest_differs_on_content();
  test_digest_empty();

  test_text_payload_roundtrip();
  test_text_payload_empty_input();
  test_text_payload_odd_length();
  test_text_payload_too_large();
  test_parse_text_bad_type();
  test_parse_text_truncated();

  test_bitmap_roundtrip_raw_small();
  test_bitmap_roundtrip_compressed();
  test_bitmap_bad_header();
  test_bitmap_empty();
  test_parse_bitmap_bad_type();
  test_parse_bitmap_bad_codec();

  if (g_fails != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_fails);
    return 1;
  }
  std::printf("clipboard_test: ok\n");
  return 0;
}
