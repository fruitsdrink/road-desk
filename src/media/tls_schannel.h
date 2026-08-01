#pragma once

// Schannel TLS for the media-plane adapter (not control-plane).
// RFB bytes travel only inside an established TLS session.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <string>

namespace road_desk::media::tls {

struct TlsSession;

struct HostCredentials {
  void* cert_context = nullptr;  // PCCERT_CONTEXT
  std::string fingerprint_sha256_hex;
};

// Create an in-memory self-signed cert for MVP host listen.
bool create_self_signed_credentials(HostCredentials* out);
void free_host_credentials(HostCredentials* creds);

// Peer cert SHA-256 fingerprint (lowercase hex), empty on failure.
std::string peer_fingerprint_sha256(TlsSession* session);

SOCKET tcp_listen(int port);
SOCKET tcp_accept(SOCKET listen_sock);
SOCKET tcp_connect(const char* host, int port);

// Takes ownership of tcp_sock on success (closes on failure / tls_close).
TlsSession* server_handshake(SOCKET tcp_sock, const HostCredentials& creds);
// expected_fingerprint_sha256_hex empty + insecure => accept any cert (debug only).
TlsSession* client_handshake(SOCKET tcp_sock, bool insecure,
                             const std::string& expected_fingerprint_sha256_hex);

int tls_read(TlsSession* session, void* buf, int len);
int tls_write(TlsSession* session, const void* buf, int len);
void tls_close(TlsSession* session);
// Underlying TCP socket (for select/poll). Do not closesocket; use tls_close.
SOCKET tls_get_socket(TlsSession* session);
// Bytes already decrypted and ready (0 if need socket recv; <0 on error).
int tls_pending(TlsSession* session);

// Connected pair bound only to loopback; not a cleartext RFB listen.
bool make_socket_pair(SOCKET* a, SOCKET* b);

struct Bridge {
  HANDLE thread = nullptr;
  volatile LONG stop = 0;
  TlsSession* tls = nullptr;
  SOCKET plain = INVALID_SOCKET;  // end owned by bridge (LibVNC owns the other)
};

// Bridge owns tls and plain; starts a pump thread. Returns false on failure
// (caller still owns tls/plain and must close them).
bool start_bridge(Bridge* bridge, TlsSession* tls, SOCKET plain);
void stop_bridge(Bridge* bridge);

bool parse_host_port(const std::string& host_port, std::string* host, int* port);

}  // namespace road_desk::media::tls
