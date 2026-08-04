#include "capture_resolve.h"
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

}  // namespace

int main() {
  test_legacy_gate();
  test_resolve_matrix();
  test_encode_solid_rect();
  if (g_fails != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_fails);
    return 1;
  }
  std::printf("capture_strategy_test: ok\n");
  return 0;
}
