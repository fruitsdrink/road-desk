#include "mux.h"

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

// ── parse_auth_password ────────────────────────────────────────────

void test_parse_auth_basic() {
  using namespace road_desk::replace;
  // Manual: kCtrlAuth(1) | u16 le pass_len(4) | "test"
  uint8_t buf[] = {kCtrlAuth, 4, 0, 't', 'e', 's', 't'};
  std::string pw;
  expect(parse_auth_password(buf, sizeof(buf), &pw), "basic parse ok");
  expect(pw == "test", "basic password match");
}

void test_parse_auth_empty_password() {
  using namespace road_desk::replace;
  uint8_t buf[] = {kCtrlAuth, 0, 0};
  std::string pw;
  expect(parse_auth_password(buf, sizeof(buf), &pw), "empty password ok");
  expect(pw.empty(), "empty password string");
}

void test_parse_auth_with_session_id() {
  using namespace road_desk::replace;
  // kCtrlAuth | u16 pass_len=4 | "pass" | u16 sid_len=4 | "sid1"
  uint8_t buf[] = {kCtrlAuth, 4, 0, 'p', 'a', 's', 's', 4, 0, 's', 'i', 'd', '1'};
  std::string pw, sid;
  expect(parse_auth_password(buf, sizeof(buf), &pw, &sid), "auth+session parse ok");
  expect(pw == "pass", "password correct");
  expect(sid == "sid1", "session_id correct");
}

void test_parse_auth_no_session_id() {
  using namespace road_desk::replace;
  uint8_t buf[] = {kCtrlAuth, 4, 0, 'p', 'a', 's', 's'};
  std::string pw, sid;
  expect(parse_auth_password(buf, sizeof(buf), &pw, &sid), "auth no sid ok");
  expect(pw == "pass", "pw");
  expect(sid.empty(), "sid empty when absent");
}

void test_parse_auth_null_input() {
  using namespace road_desk::replace;
  std::string pw;
  expect(!parse_auth_password(nullptr, 10, &pw), "null buffer fails");
  expect(!parse_auth_password(nullptr, 10, &pw, nullptr), "null buffer fails 2");
}

void test_parse_auth_null_password_out() {
  using namespace road_desk::replace;
  uint8_t buf[] = {kCtrlAuth, 1, 0, 'x'};
  expect(!parse_auth_password(buf, sizeof(buf), nullptr), "null pw out fails");
}

void test_parse_auth_too_short() {
  using namespace road_desk::replace;
  uint8_t buf[] = {kCtrlAuth, 10, 0};  // claims 10 bytes but only has 0
  std::string pw;
  expect(!parse_auth_password(buf, sizeof(buf), &pw), "too short fails");
}

void test_parse_auth_wrong_type() {
  using namespace road_desk::replace;
  uint8_t buf[] = {kCtrlAuthOk, 4, 0, 't', 'e', 's', 't'};
  std::string pw;
  expect(!parse_auth_password(buf, sizeof(buf), &pw), "wrong type fails");
}

void test_parse_auth_long_session_id() {
  using namespace road_desk::replace;
  // Session ID > 128 is silently ignored (not written to output), parse still succeeds.
  std::vector<uint8_t> buf;
  buf.push_back(kCtrlAuth);
  buf.push_back(1); buf.push_back(0);  // pass len = 1
  buf.push_back('x');
  buf.push_back(130); buf.push_back(0);  // sid len = 130
  for (int i = 0; i < 130; ++i) buf.push_back('a');
  std::string pw, sid;
  expect(parse_auth_password(buf.data(), buf.size(), &pw, &sid),
         "oversized session id: parse still ok");
  expect(pw == "x", "password correct despite oversized sid");
  expect(sid.empty(), "session_id ignored when oversized");
}

void test_parse_auth_max_password() {
  using namespace road_desk::replace;
  std::string big(1024, 'x');
  std::vector<uint8_t> buf;
  buf.push_back(kCtrlAuth);
  buf.push_back(0); buf.push_back(4);  // 1024 LE
  buf.insert(buf.end(), big.begin(), big.end());
  std::string pw;
  expect(parse_auth_password(buf.data(), buf.size(), &pw), "max password ok");
  expect(pw == big, "max password correct");
}

// ── parse_auth_ok ──────────────────────────────────────────────────

void test_parse_auth_ok_basic() {
  using namespace road_desk::replace;
  // kCtrlAuthOk | u16 w=1920 | u16 h=1080
  uint8_t buf[] = {kCtrlAuthOk, 0x80, 0x07, 0x38, 0x04};  // 1920, 1080 LE
  uint16_t w = 0, h = 0;
  expect(parse_auth_ok(buf, sizeof(buf), &w, &h), "auth ok basic");
  expect(w == 1920, "width 1920");
  expect(h == 1080, "height 1080");
}

void test_parse_auth_ok_with_version() {
  using namespace road_desk::replace;
  // kCtrlAuthOk | w | h | u16 vlen=4 | "0.1.2" | session_role
  uint8_t buf[] = {kCtrlAuthOk, 0x80, 0x07, 0x38, 0x04,
                   4, 0, '0', '.', '1', '2', kSessionRoleControl};
  uint16_t w = 0, h = 0;
  std::string ver;
  uint8_t role = 99;
  expect(parse_auth_ok(buf, sizeof(buf), &w, &h, &ver, &role), "auth ok + ver + role");
  expect(w == 1920, "w");
  expect(h == 1080, "h");
  expect(ver == "0.12", "version");
  expect(role == kSessionRoleControl, "role control");
}

void test_parse_auth_ok_view_only() {
  using namespace road_desk::replace;
  uint8_t buf[] = {kCtrlAuthOk, 0x80, 0x07, 0x38, 0x04,
                   0, 0, kSessionRoleViewOnly};
  uint16_t w = 0, h = 0;
  uint8_t role = 99;
  expect(parse_auth_ok(buf, sizeof(buf), &w, &h, nullptr, &role), "auth ok view_only");
  expect(role == kSessionRoleViewOnly, "role view only");
}

void test_parse_auth_ok_old_host_no_role() {
  using namespace road_desk::replace;
  // Old host sends only w/h, no version, no role byte.
  uint8_t buf[] = {kCtrlAuthOk, 0x80, 0x07, 0x38, 0x04};
  uint16_t w = 0, h = 0;
  uint8_t role = 99;
  expect(parse_auth_ok(buf, sizeof(buf), &w, &h, nullptr, &role), "old host ok");
  expect(w == 1920, "w");
  expect(h == 1080, "h");
  // When no role byte is present, defaults to control.
  expect(role == kSessionRoleControl, "old host default control");
}

void test_parse_auth_ok_no_version_but_role() {
  using namespace road_desk::replace;
  // Version length 0, then role byte.
  uint8_t buf[] = {kCtrlAuthOk, 0x80, 0x07, 0x38, 0x04,
                   0, 0, kSessionRoleViewOnly};
  uint16_t w = 0, h = 0;
  std::string ver;
  expect(parse_auth_ok(buf, sizeof(buf), &w, &h, &ver), "no version but has trailing");
  expect(ver.empty(), "version empty");
}

void test_parse_auth_ok_too_short() {
  using namespace road_desk::replace;
  uint8_t buf[] = {kCtrlAuthOk, 0x80, 0x07};  // incomplete
  uint16_t w = 0, h = 0;
  expect(!parse_auth_ok(buf, sizeof(buf), &w, &h), "too short fails");
}

void test_parse_auth_ok_null_outputs() {
  using namespace road_desk::replace;
  uint8_t buf[] = {kCtrlAuthOk, 0x80, 0x07, 0x38, 0x04};
  expect(!parse_auth_ok(buf, sizeof(buf), nullptr, nullptr), "null w/h fails");
}

void test_parse_auth_ok_truncated_version() {
  using namespace road_desk::replace;
  // vlen says 10 bytes but only 3 follow.
  uint8_t buf[] = {kCtrlAuthOk, 0x80, 0x07, 0x38, 0x04, 10, 0, 'a', 'b', 'c'};
  uint16_t w = 0, h = 0;
  expect(!parse_auth_ok(buf, sizeof(buf), &w, &h), "truncated version fails");
}

// ── Enum constant checks ───────────────────────────────────────────

void test_channel_values() {
  using namespace road_desk::replace;
  expect(kChannelControl == 1, "control channel");
  expect(kChannelInput == 2, "input channel");
  expect(kChannelVideo == 3, "video channel");
  expect(kChannelCursor == 4, "cursor channel");
  expect(kChannelClipboard == 5, "clipboard channel");
  expect(kChannelFile == 6, "file channel");
}

void test_control_type_values() {
  using namespace road_desk::replace;
  expect(kCtrlAuth == 1, "auth");
  expect(kCtrlAuthOk == 2, "auth ok");
  expect(kCtrlAuthFail == 3, "auth fail");
  expect(kCtrlPing == 4, "ping");
  expect(kCtrlPong == 5, "pong");
  expect(kCtrlSessionRole == 6, "session role");
}

void test_header_size_constants() {
  using namespace road_desk::replace;
  expect(kVideoHeaderSize == 1 + 4 + 2 * 4, "video header size");
  expect(kInputPointerSize == 1 + 1 + 2 + 2, "pointer size");
  expect(kInputKeySize == 1 + 1 + 2, "key size");
}

void test_session_role_values() {
  using namespace road_desk::replace;
  expect(kSessionRoleControl == 0, "role control");
  expect(kSessionRoleViewOnly == 1, "role view only");
}

}  // namespace

int main() {
  test_parse_auth_basic();
  test_parse_auth_empty_password();
  test_parse_auth_with_session_id();
  test_parse_auth_no_session_id();
  test_parse_auth_null_input();
  test_parse_auth_null_password_out();
  test_parse_auth_too_short();
  test_parse_auth_wrong_type();
  test_parse_auth_long_session_id();
  test_parse_auth_max_password();

  test_parse_auth_ok_basic();
  test_parse_auth_ok_with_version();
  test_parse_auth_ok_view_only();
  test_parse_auth_ok_old_host_no_role();
  test_parse_auth_ok_no_version_but_role();
  test_parse_auth_ok_too_short();
  test_parse_auth_ok_null_outputs();
  test_parse_auth_ok_truncated_version();

  test_channel_values();
  test_control_type_values();
  test_header_size_constants();
  test_session_role_values();

  if (g_fails != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_fails);
    return 1;
  }
  std::printf("mux_parse_test: ok\n");
  return 0;
}
