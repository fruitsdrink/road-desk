#include "tls_schannel.h"

#include <cstdio>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace {

int g_fails = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_fails;
  }
}

void test_create_self_signed_credentials() {
  using namespace road_desk::media::tls;
  HostCredentials creds;
  bool ok = create_self_signed_credentials(&creds);
  expect(ok, "create self-signed creds ok");
  if (!ok) {
    return;
  }
  expect(creds.cert_context != nullptr, "cert context not null");
  expect(!creds.fingerprint_sha256_hex.empty(), "fingerprint not empty");
  expect(creds.fingerprint_sha256_hex.size() == 64, "SHA-256 hex = 64 chars");
  for (char c : creds.fingerprint_sha256_hex) {
    expect((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'), "fingerprint is hex");
  }
  free_host_credentials(&creds);
  expect(creds.cert_context == nullptr, "cert freed");
}

void test_create_credentials_idempotent() {
  using namespace road_desk::media::tls;
  HostCredentials a{}, b{};
  bool oka = create_self_signed_credentials(&a);
  bool okb = create_self_signed_credentials(&b);
  expect(oka && okb, "two creates ok");
  if (oka && okb) {
    expect(!a.fingerprint_sha256_hex.empty(), "a fingerprint");
    expect(!b.fingerprint_sha256_hex.empty(), "b fingerprint");
  }
  free_host_credentials(&a);
  free_host_credentials(&b);
}

void test_make_socket_pair() {
  using namespace road_desk::media::tls;
  SOCKET a = INVALID_SOCKET;
  SOCKET b = INVALID_SOCKET;
  bool ok = make_socket_pair(&a, &b);
  expect(ok, "socket pair ok");
  if (!ok) return;
  expect(a != INVALID_SOCKET, "a valid");
  expect(b != INVALID_SOCKET, "b valid");
  const char msg[] = "hello";
  int sent = send(a, msg, static_cast<int>(sizeof(msg)), 0);
  expect(sent == static_cast<int>(sizeof(msg)), "send ok");
  char buf[16] = {};
  int got = recv(b, buf, sizeof(buf), 0);
  expect(got == static_cast<int>(sizeof(msg)), "recv ok");
  expect(std::string(buf) == std::string(msg), "data roundtrip");
  closesocket(a);
  closesocket(b);
}

// ── Threaded handshake helpers ──────────────────────────────────

struct HandshakeResult {
  road_desk::media::tls::TlsSession* tls = nullptr;
  bool done = false;
};

void server_handshake_thread(road_desk::media::tls::HostCredentials* creds,
                             SOCKET sock, HandshakeResult* out) {
  out->tls = road_desk::media::tls::server_handshake(sock, *creds);
  out->done = true;
}

void client_handshake_thread(bool insecure, const std::string& fp,
                             SOCKET sock, HandshakeResult* out) {
  out->tls = road_desk::media::tls::client_handshake(sock, insecure, fp);
  out->done = true;
}

// ── TLS handshake tests (threaded) ──────────────────────────────

void test_tls_handshake_loopback() {
  using namespace road_desk::media::tls;
  HostCredentials creds{};
  if (!create_self_signed_credentials(&creds)) {
    std::fprintf(stderr, "SKIP: tls_handshake_loopback — cert creation failed\n");
    return;
  }

  SOCKET srv = INVALID_SOCKET;
  SOCKET cli = INVALID_SOCKET;
  if (!make_socket_pair(&srv, &cli)) {
    std::fprintf(stderr, "SKIP: tls_handshake_loopback — socket pair failed\n");
    free_host_credentials(&creds);
    return;
  }

  HandshakeResult srv_res{}, cli_res{};
  std::thread st(server_handshake_thread, &creds, srv, &srv_res);
  std::thread ct(client_handshake_thread, false,
                 creds.fingerprint_sha256_hex, cli, &cli_res);
  st.join();
  ct.join();

  expect(srv_res.tls != nullptr, "server handshake ok");
  expect(cli_res.tls != nullptr, "client handshake ok");

  if (srv_res.tls && cli_res.tls) {
    std::string client_peer = peer_fingerprint_sha256(cli_res.tls);
    expect(!client_peer.empty(), "client peer fingerprint");
    expect(client_peer == creds.fingerprint_sha256_hex,
           "client sees host fingerprint");

    const char* msg = "road-desk-tls-test";
    int wlen = static_cast<int>(std::strlen(msg));
    int wr = tls_write(cli_res.tls, msg, wlen);
    expect(wr == wlen, "tls write ok");

    char buf[128] = {};
    int rd = tls_read(srv_res.tls, buf, sizeof(buf));
    expect(rd == wlen, "tls read ok");
    expect(std::string(buf, static_cast<size_t>(rd)) == std::string(msg),
           "tls data roundtrip");

    const char* reply = "ack";
    int rw = static_cast<int>(std::strlen(reply));
    tls_write(srv_res.tls, reply, rw);
    char buf2[16] = {};
    int rd2 = tls_read(cli_res.tls, buf2, sizeof(buf2));
    expect(rd2 == rw, "reverse read ok");
    expect(std::string(buf2, static_cast<size_t>(rd2)) == std::string(reply),
           "reverse roundtrip");
  }

  tls_close(srv_res.tls);
  tls_close(cli_res.tls);
  free_host_credentials(&creds);
}

void test_tls_client_insecure_mode() {
  using namespace road_desk::media::tls;
  HostCredentials creds{};
  if (!create_self_signed_credentials(&creds)) {
    std::fprintf(stderr, "SKIP: tls_insecure — cert creation failed\n");
    return;
  }

  SOCKET srv = INVALID_SOCKET;
  SOCKET cli = INVALID_SOCKET;
  if (!make_socket_pair(&srv, &cli)) {
    free_host_credentials(&creds);
    return;
  }

  HandshakeResult srv_res{}, cli_res{};
  std::thread st(server_handshake_thread, &creds, srv, &srv_res);
  std::thread ct(client_handshake_thread, true, "", cli, &cli_res);
  st.join();
  ct.join();

  expect(srv_res.tls != nullptr, "server ok (insecure test)");
  expect(cli_res.tls != nullptr, "client ok in insecure mode");

  tls_close(srv_res.tls);
  tls_close(cli_res.tls);
  free_host_credentials(&creds);
}

void test_tls_client_wrong_fingerprint() {
  using namespace road_desk::media::tls;
  HostCredentials creds{};
  if (!create_self_signed_credentials(&creds)) {
    std::fprintf(stderr, "SKIP: tls_wrong_fp — cert creation failed\n");
    return;
  }

  SOCKET srv = INVALID_SOCKET;
  SOCKET cli = INVALID_SOCKET;
  if (!make_socket_pair(&srv, &cli)) {
    free_host_credentials(&creds);
    return;
  }

  std::string wrong_fp(64, '0');
  HandshakeResult srv_res{}, cli_res{};
  std::thread st(server_handshake_thread, &creds, srv, &srv_res);
  std::thread ct(client_handshake_thread, false, wrong_fp, cli, &cli_res);
  st.join();
  ct.join();

  // Client handshake should fail because the server cert doesn't match.
  expect(cli_res.tls == nullptr, "client rejected on wrong fingerprint");

  tls_close(srv_res.tls);
  tls_close(cli_res.tls);
  free_host_credentials(&creds);
}

void test_parse_host_port() {
  using namespace road_desk::media::tls;
  std::string host;
  int port = 0;

  expect(parse_host_port("192.168.1.1:38471", &host, &port), "parse ip:port");
  expect(host == "192.168.1.1", "host");
  expect(port == 38471, "port");

  expect(parse_host_port("localhost:1234", &host, &port), "parse localhost:port");
  expect(host == "localhost", "host localhost");
  expect(port == 1234, "port localhost");

  expect(parse_host_port("host.name:9999", &host, &port), "parse hostname:port");
  expect(host == "host.name", "hostname");
  expect(port == 9999, "port hostname");

  expect(!parse_host_port("noport", &host, &port), "no colon fails");
  expect(!parse_host_port(":1234", &host, &port), "empty host fails");
  expect(!parse_host_port("host:", &host, &port), "empty port fails");
  expect(!parse_host_port("host:abc", &host, &port), "non-numeric port fails");
  expect(!parse_host_port("host:0", &host, &port), "port 0 fails");
  expect(!parse_host_port("host:65536", &host, &port), "port >65535 fails");
}

}  // namespace

int main() {
#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

  test_create_self_signed_credentials();
  test_create_credentials_idempotent();
  test_make_socket_pair();
  test_tls_handshake_loopback();
  test_tls_client_insecure_mode();
  test_tls_client_wrong_fingerprint();
  test_parse_host_port();

#ifdef _WIN32
  WSACleanup();
#endif

  if (g_fails != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_fails);
    return 1;
  }
  std::printf("tls_test: ok\n");
  return 0;
}
