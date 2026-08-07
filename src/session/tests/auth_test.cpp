#include "auth.h"

#include <cstdio>
#include <string>

namespace {

int g_fails = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_fails;
  }
}

void test_exact_match() {
  using namespace road_desk::session;
  expect(authenticate_psk("secret123", "secret123"), "exact match");
}

void test_wrong_password() {
  using namespace road_desk::session;
  expect(!authenticate_psk("secret123", "wrongpass"), "wrong password");
}

void test_empty_expected_fails() {
  using namespace road_desk::session;
  // Empty expected password must never authenticate (fail closed).
  expect(!authenticate_psk("", ""), "empty both fails");
  expect(!authenticate_psk("", "anything"), "empty expected fails");
}

void test_empty_provided() {
  using namespace road_desk::session;
  expect(!authenticate_psk("secret123", ""), "empty provided fails");
}

void test_single_char_diff() {
  using namespace road_desk::session;
  expect(!authenticate_psk("abcdefgh", "abcdefgz"), "last char diff");
  expect(!authenticate_psk("abcdefgh", "zbcdefgh"), "first char diff");
  expect(!authenticate_psk("abcdefgh", "abcDefgh"), "case diff");
}

void test_length_diff() {
  using namespace road_desk::session;
  expect(!authenticate_psk("secret123", "secret1234"), "provided longer");
  expect(!authenticate_psk("secret123", "secret12"), "provided shorter");
}

void test_prefix_match_fails() {
  using namespace road_desk::session;
  // Provided is a prefix of expected — must fail (length participates in diff).
  expect(!authenticate_psk("secret123", "secret"), "prefix match fails");
}

void test_long_passwords() {
  using namespace road_desk::session;
  std::string long_pwd(1000, 'x');
  expect(authenticate_psk(long_pwd, long_pwd), "long exact match (1000)");
  expect(!authenticate_psk(long_pwd, long_pwd + "y"), "long diff");
}

void test_special_chars() {
  using namespace road_desk::session;
  const std::string pwd("\x00\x01\x02\xff", 4);
  expect(authenticate_psk(pwd, pwd), "binary exact match");
  const std::string wrong("\x00\x01\x03\xff", 4);
  expect(!authenticate_psk(pwd, wrong), "binary one-byte diff");
}

void test_leading_null() {
  using namespace road_desk::session;
  // std::string can hold embedded NUL bytes.
  const std::string pwd("\x00visible", 8);
  const std::string wrong("\x00visiblE", 8);
  expect(authenticate_psk(pwd, pwd), "embedded nul match");
  expect(!authenticate_psk(pwd, wrong), "embedded nul diff");
}

}  // namespace

int main() {
  test_exact_match();
  test_wrong_password();
  test_empty_expected_fails();
  test_empty_provided();
  test_single_char_diff();
  test_length_diff();
  test_prefix_match_fails();
  test_long_passwords();
  test_special_chars();
  test_leading_null();

  if (g_fails != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_fails);
    return 1;
  }
  std::printf("auth_test: ok\n");
  return 0;
}
