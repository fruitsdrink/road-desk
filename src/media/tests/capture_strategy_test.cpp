#include "capture_resolve.h"
#include "h264_mf.h"
#include "mux_protocol.h"
#include "video_encode.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_fails = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_fails;
  }
}

road_desk::replace::OsVersionInfo ver(unsigned major, unsigned minor, unsigned build,
                                      bool ok = true) {
  road_desk::replace::OsVersionInfo v;
  v.major = major;
  v.minor = minor;
  v.build = build;
  v.ok = ok;
  return v;
}

void test_legacy_gate() {
  using namespace road_desk::replace;
  expect(is_legacy_capture_host(ver(6, 0, 6001)), "Server 2008 is legacy");
  expect(is_legacy_capture_host(ver(6, 1, 7601)), "Win7 is legacy");
  expect(!is_legacy_capture_host(ver(6, 2, 9200)), "Win8 / 2012 is modern");
  expect(!is_legacy_capture_host(ver(6, 3, 9600)), "8.1 is modern");
  expect(!is_legacy_capture_host(ver(10, 0, 19045)), "Win10 is modern");
  expect(is_legacy_capture_host(ver(0, 0, 0, false)), "query fail => legacy");
  expect(capture_strategy_for(ver(6, 0, 6001)) == CaptureStrategy::Legacy, "6.0 strategy");
  expect(capture_strategy_for(ver(10, 0, 26100)) == CaptureStrategy::Modern, "10 strategy");
}

void test_resolve_matrix() {
  using namespace road_desk::replace;
  {
    const auto r = resolve_capture_for(ver(6, 0, 6001), nullptr);
    expect(r.strategy == CaptureStrategy::Legacy, "2008 strategy");
    expect(r.requested == CaptureMode::Auto, "2008 requested auto");
    expect(r.effective == CaptureMode::Auto, "2008 auto stays auto (Mirror path)");
  }
  {
    const auto r = resolve_capture_for(ver(6, 1, 7601), "auto");
    expect(r.effective == CaptureMode::Auto, "Win7 auto stays auto");
  }
  {
    const auto r = resolve_capture_for(ver(10, 0, 26100), "auto");
    expect(r.strategy == CaptureStrategy::Modern, "Win10 modern");
    expect(r.effective == CaptureMode::Dxgi, "Win10 auto => dxgi");
    expect(r.note && std::strstr(r.note, "dxgi") != nullptr, "Win10 note");
  }
  {
    const auto r = resolve_capture_for(ver(6, 2, 9200), nullptr);
    expect(r.effective == CaptureMode::Dxgi, "2012 auto => dxgi");
  }
  {
    const auto r = resolve_capture_for(ver(6, 1, 7601), "dxgi");
    expect(r.effective == CaptureMode::Gdi, "legacy dxgi => gdi");
    expect(r.note && std::strstr(r.note, "forbidden") != nullptr, "legacy dxgi note");
  }
  {
    const auto r = resolve_capture_for(ver(10, 0, 19045), "dxgi");
    expect(r.effective == CaptureMode::Dxgi, "modern dxgi kept");
  }
  {
    const auto r = resolve_capture_for(ver(10, 0, 19045), "mirror");
    expect(r.effective == CaptureMode::Mirror, "force mirror on modern kept");
  }
  {
    const auto r = resolve_capture_for(ver(6, 1, 7601), "gdi");
    expect(r.effective == CaptureMode::Gdi, "force gdi");
  }
  {
    const auto r = resolve_capture_for(ver(0, 0, 0, false), "auto");
    expect(r.strategy == CaptureStrategy::Legacy, "fail => legacy");
    expect(r.effective == CaptureMode::Auto, "fail auto stays auto");
  }
  expect(parse_capture_mode("DXGI") == CaptureMode::Dxgi, "parse dxgi case-insensitive");
  expect(std::strcmp(capture_mode_name(CaptureMode::Dxgi), "dxgi") == 0, "name dxgi");
}

void test_encode_solid_rect() {
  using namespace road_desk::media;
  using road_desk::replace::kVideoHeaderSize;
  using road_desk::replace::kVideoRawBgra;
  using road_desk::replace::kVideoZlibBgra;
  constexpr int W = 64;
  constexpr int H = 64;
  std::vector<uint8_t> bgra(static_cast<size_t>(W) * H * 4, 0);
  for (size_t i = 0; i < bgra.size(); i += 4) {
    bgra[i] = 40;
    bgra[i + 1] = 80;
    bgra[i + 2] = 120;
    bgra[i + 3] = 255;
  }
  std::vector<uint8_t> raw;
  std::vector<uint8_t> body;
  EncodeRectStats st{};
  expect(encode_video_rect(7, W, H, 0, 0, W, H, bgra.data(), &raw, &body, &st), "encode ok");
  expect(st.raw_bytes == static_cast<uint32_t>(W * H * 4), "raw size");
  expect(st.wire_bytes == body.size(), "wire matches body");
  expect(st.wire_bytes >= kVideoHeaderSize, "has header");
  expect(body[0] == kVideoZlibBgra || body[0] == kVideoRawBgra, "codec id");
  // Solid fill should zlib well.
  expect(body[0] == kVideoZlibBgra, "solid prefers zlib");
  expect(st.wire_bytes < st.raw_bytes + kVideoHeaderSize, "zlib smaller than raw+hdr bound");
  expect(body[1] == 7 && body[2] == 0 && body[3] == 0 && body[4] == 0, "frame_id le");
}

void test_h264_roundtrip() {
  using namespace road_desk::media;
  using road_desk::replace::kVideoHeaderSize;
  using road_desk::replace::kVideoH264;
  constexpr int W = 128;
  constexpr int H = 96;
  std::vector<uint8_t> bgra(static_cast<size_t>(W) * H * 4, 0);
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      uint8_t* p = bgra.data() + (static_cast<size_t>(y) * W + x) * 4u;
      p[0] = static_cast<uint8_t>(x * 2);          // B
      p[1] = static_cast<uint8_t>(y * 2);          // G
      p[2] = static_cast<uint8_t>((x + y) & 0xff); // R
      p[3] = 255;
    }
  }
  // Solid patch for easier MAE check.
  for (int y = 16; y < 48; ++y) {
    for (int x = 16; x < 80; ++x) {
      uint8_t* p = bgra.data() + (static_cast<size_t>(y) * W + x) * 4u;
      p[0] = 30;
      p[1] = 180;
      p[2] = 220;
      p[3] = 255;
    }
  }

  std::vector<uint8_t> nv12;
  std::vector<uint8_t> body;
  EncodeRectStats st{};
  if (!encode_h264_rect(1, W, H, 0, 0, W, H, bgra.data(), 70, &nv12, &body, &st)) {
    std::printf("h264_roundtrip: SKIP (MF H.264 encode unavailable)\n");
    h264_codec_shutdown();
    return;
  }
  expect(body[0] == kVideoH264, "h264 codec id");
  expect(body.size() > kVideoHeaderSize, "h264 payload");
  expect(st.codec == kVideoH264, "h264 stats codec");

  std::vector<uint8_t> decoded;
  const uint8_t* annex = body.data() + kVideoHeaderSize;
  const size_t annex_len = body.size() - kVideoHeaderSize;
  if (!decode_h264_to_bgra(annex, annex_len, W, H, &decoded)) {
    expect(false, "h264 decode");
    h264_codec_shutdown();
    return;
  }
  expect(decoded.size() == bgra.size(), "h264 decode size");

  // Mean abs error on RGB (ignore A). Lossy; keep a loose threshold.
  uint64_t abs_sum = 0;
  const size_t pix = static_cast<size_t>(W) * H;
  for (size_t i = 0; i < pix; ++i) {
    const uint8_t* a = bgra.data() + i * 4;
    const uint8_t* b = decoded.data() + i * 4;
    abs_sum += static_cast<uint64_t>(a[0] > b[0] ? a[0] - b[0] : b[0] - a[0]);
    abs_sum += static_cast<uint64_t>(a[1] > b[1] ? a[1] - b[1] : b[1] - a[1]);
    abs_sum += static_cast<uint64_t>(a[2] > b[2] ? a[2] - b[2] : b[2] - a[2]);
  }
  const double mae = static_cast<double>(abs_sum) / static_cast<double>(pix * 3u);
  std::printf("h264_roundtrip: mae=%.2f wire=%u\n", mae, st.wire_bytes);
  expect(mae < 25.0, "h264 mae under threshold");
  // Shredding typically yields huge MAE or chroma stripes; also spot-check solid patch mean.
  uint64_t patch = 0;
  size_t patch_n = 0;
  for (int y = 20; y < 44; ++y) {
    for (int x = 20; x < 76; ++x) {
      const uint8_t* b = decoded.data() + (static_cast<size_t>(y) * W + x) * 4u;
      patch += b[0] + b[1] + b[2];
      ++patch_n;
    }
  }
  const double patch_mean = static_cast<double>(patch) / static_cast<double>(patch_n * 3u);
  expect(patch_mean > 80.0 && patch_mean < 200.0, "h264 solid patch not shredded");
  // Non-16 size expands upward within the desk (100x68 → 112x80).
  {
    constexpr int DeskW = 128;
    constexpr int DeskH = 96;
    constexpr int X = 0;
    constexpr int Y = 0;
    constexpr int W2 = 100;
    constexpr int H2 = 68;
    std::vector<uint8_t> desk(static_cast<size_t>(DeskW) * DeskH * 4, 90);
    for (size_t i = 3; i < desk.size(); i += 4) {
      desk[i] = 255;
    }
    std::vector<uint8_t> nv12b;
    std::vector<uint8_t> body2;
    EncodeRectStats st2{};
    if (encode_h264_rect(2, DeskW, DeskH, X, Y, W2, H2, desk.data(), 70, &nv12b, &body2, &st2)) {
      const int cw = body2[9] | (body2[10] << 8);
      const int ch = body2[11] | (body2[12] << 8);
      expect(cw == 112 && ch == 80, "h264 expands to 16-align");
      std::vector<uint8_t> dec2;
      expect(decode_h264_to_bgra(body2.data() + kVideoHeaderSize, body2.size() - kVideoHeaderSize, cw,
                                 ch, &dec2),
             "h264 decode expanded");
    }
  }
  h264_codec_shutdown();
}

}  // namespace

int main() {
  test_legacy_gate();
  test_resolve_matrix();
  test_encode_solid_rect();
  test_h264_roundtrip();
  if (g_fails != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_fails);
    return 1;
  }
  std::printf("capture_strategy_test: ok\n");
  return 0;
}
