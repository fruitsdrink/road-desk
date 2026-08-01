#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06010000
#endif

#include "tls_schannel.h"

#define SECURITY_WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <wincrypt.h>
#include <ncrypt.h>
#include <schannel.h>
#include <security.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ncrypt.lib")

namespace road_desk::media::tls {

// From tls_mvp_cert_data.cpp — MVP embedded self-signed host PFX.
extern const unsigned char k_mvp_host_pfx[];
extern const size_t k_mvp_host_pfx_size;
extern const char k_mvp_host_pfx_password[];
extern const char k_mvp_host_fingerprint_sha256[];

struct TlsSession {
  SOCKET sock = INVALID_SOCKET;
  CredHandle cred{};
  bool cred_valid = false;
  CtxtHandle ctx{};
  bool ctx_valid = false;
  SecPkgContext_StreamSizes sizes{};
  bool sizes_valid = false;
  std::vector<char> recv_encrypted;
  std::vector<char> recv_decrypted;
  size_t decrypted_off = 0;
  PCCERT_CONTEXT peer_cert = nullptr;
  bool server = false;
};

namespace {

constexpr size_t kIoBuf = 16 * 1024;

bool send_all(SOCKET s, const char* data, int len) {
  int sent = 0;
  while (sent < len) {
    const int n = send(s, data + sent, len - sent, 0);
    if (n == SOCKET_ERROR || n == 0) {
      return false;
    }
    sent += n;
  }
  return true;
}

int recv_some(SOCKET s, char* data, int len) {
  return recv(s, data, len, 0);
}

std::string sha256_hex(const BYTE* data, DWORD len) {
  HCRYPTPROV prov = 0;
  HCRYPTHASH hash = 0;
  std::string out;
  if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
    return out;
  }
  if (!CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
    CryptReleaseContext(prov, 0);
    return out;
  }
  if (!CryptHashData(hash, data, len, 0)) {
    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
    return out;
  }
  DWORD hash_len = 0;
  DWORD hl_size = sizeof(hash_len);
  if (!CryptGetHashParam(hash, HP_HASHSIZE, reinterpret_cast<BYTE*>(&hash_len), &hl_size, 0) ||
      hash_len == 0) {
    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
    return out;
  }
  std::vector<BYTE> digest(hash_len);
  if (!CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &hash_len, 0)) {
    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
    return out;
  }
  CryptDestroyHash(hash);
  CryptReleaseContext(prov, 0);
  out.resize(hash_len * 2);
  for (DWORD i = 0; i < hash_len; ++i) {
    std::snprintf(&out[i * 2], 3, "%02x", digest[i]);
  }
  return out;
}

std::string cert_fingerprint(PCCERT_CONTEXT cert) {
  if (!cert) {
    return {};
  }
  return sha256_hex(cert->pbCertEncoded, cert->cbCertEncoded);
}

bool eq_fingerprint(const std::string& a, const std::string& b) {
  if (a.size() != b.size() || a.empty()) {
    return false;
  }
  volatile unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    const auto ca = static_cast<unsigned char>(a[i] | 0x20);
    const auto cb = static_cast<unsigned char>(b[i] | 0x20);
    diff = static_cast<unsigned char>(diff | (ca ^ cb));
  }
  return diff == 0;
}

bool init_server_cred(const HostCredentials& host, CredHandle* cred) {
  auto* cert = static_cast<PCCERT_CONTEXT>(host.cert_context);
  if (!cert) {
    return false;
  }
  SCHANNEL_CRED sch{};
  sch.dwVersion = SCHANNEL_CRED_VERSION;
  sch.cCreds = 1;
  sch.paCred = &cert;
  sch.grbitEnabledProtocols = SP_PROT_TLS1_0 | SP_PROT_TLS1_1 | SP_PROT_TLS1_2;
  sch.dwFlags = SCH_CRED_NO_SYSTEM_MAPPER | SCH_CRED_NO_DEFAULT_CREDS;

  TimeStamp expiry{};
  const SECURITY_STATUS st =
      AcquireCredentialsHandleW(nullptr, const_cast<SEC_WCHAR*>(UNISP_NAME_W), SECPKG_CRED_INBOUND,
                                nullptr, &sch, nullptr, nullptr, cred, &expiry);
  return st == SEC_E_OK;
}

bool init_client_cred(CredHandle* cred) {
  SCHANNEL_CRED sch{};
  sch.dwVersion = SCHANNEL_CRED_VERSION;
  sch.grbitEnabledProtocols = SP_PROT_TLS1_0 | SP_PROT_TLS1_1 | SP_PROT_TLS1_2;
  // Manual trust via fingerprint (or insecure skip); do not use system CA store.
  sch.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_NO_SERVERNAME_CHECK;

  TimeStamp expiry{};
  const SECURITY_STATUS st =
      AcquireCredentialsHandleW(nullptr, const_cast<SEC_WCHAR*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND,
                                nullptr, &sch, nullptr, nullptr, cred, &expiry);
  return st == SEC_E_OK;
}

bool query_sizes(TlsSession* s) {
  const SECURITY_STATUS st =
      QueryContextAttributesW(&s->ctx, SECPKG_ATTR_STREAM_SIZES, &s->sizes);
  s->sizes_valid = (st == SEC_E_OK);
  return s->sizes_valid;
}

bool capture_peer_cert(TlsSession* s) {
  PCCERT_CONTEXT cert = nullptr;
  const SECURITY_STATUS st =
      QueryContextAttributesW(&s->ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert);
  if (st != SEC_E_OK || !cert) {
    return false;
  }
  if (s->peer_cert) {
    CertFreeCertificateContext(s->peer_cert);
  }
  s->peer_cert = cert;
  return true;
}

SECURITY_STATUS send_token(SOCKET sock, SecBuffer* token) {
  if (!token || token->cbBuffer == 0 || !token->pvBuffer) {
    return SEC_E_OK;
  }
  if (!send_all(sock, static_cast<const char*>(token->pvBuffer), static_cast<int>(token->cbBuffer))) {
    return SEC_E_INTERNAL_ERROR;
  }
  FreeContextBuffer(token->pvBuffer);
  token->pvBuffer = nullptr;
  token->cbBuffer = 0;
  return SEC_E_OK;
}

TlsSession* complete_handshake_server(SOCKET sock, CredHandle* cred) {
  auto* s = new TlsSession();
  s->sock = sock;
  s->cred = *cred;
  s->cred_valid = true;
  s->server = true;
  s->recv_encrypted.reserve(kIoBuf);

  DWORD flags_in = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
                   ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM;
  DWORD flags_out = 0;
  bool first = true;
  std::vector<char> inbuf;
  inbuf.reserve(kIoBuf);

  for (;;) {
    if (!first || inbuf.empty()) {
      char tmp[kIoBuf];
      const int n = recv_some(sock, tmp, sizeof(tmp));
      if (n <= 0) {
        tls_close(s);
        return nullptr;
      }
      inbuf.insert(inbuf.end(), tmp, tmp + n);
    }
    first = false;

    SecBuffer in_bufs[2]{};
    in_bufs[0].BufferType = SECBUFFER_TOKEN;
    in_bufs[0].pvBuffer = inbuf.data();
    in_bufs[0].cbBuffer = static_cast<ULONG>(inbuf.size());
    in_bufs[1].BufferType = SECBUFFER_EMPTY;
    SecBufferDesc in_desc{SECBUFFER_VERSION, 2, in_bufs};

    SecBuffer out_bufs[1]{};
    out_bufs[0].BufferType = SECBUFFER_TOKEN;
    SecBufferDesc out_desc{SECBUFFER_VERSION, 1, out_bufs};

    TimeStamp expiry{};
    const SECURITY_STATUS st =
        AcceptSecurityContext(&s->cred, s->ctx_valid ? &s->ctx : nullptr, &in_desc, flags_in, 0,
                              &s->ctx, &out_desc, &flags_out, &expiry);
    s->ctx_valid = true;

    if (out_bufs[0].cbBuffer && out_bufs[0].pvBuffer) {
      if (send_token(sock, &out_bufs[0]) != SEC_E_OK) {
        tls_close(s);
        return nullptr;
      }
    }

    if (st == SEC_E_INCOMPLETE_MESSAGE) {
      // Keep accumulated bytes; wait for more from the wire.
      continue;
    }

    if (in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer > 0) {
      const size_t extra = in_bufs[1].cbBuffer;
      std::memmove(inbuf.data(), inbuf.data() + (inbuf.size() - extra), extra);
      inbuf.resize(extra);
    } else {
      inbuf.clear();
    }

    if (st == SEC_E_OK) {
      if (!query_sizes(s)) {
        tls_close(s);
        return nullptr;
      }
      capture_peer_cert(s);
      if (!inbuf.empty()) {
        s->recv_encrypted.insert(s->recv_encrypted.end(), inbuf.begin(), inbuf.end());
      }
      return s;
    }
    if (st == SEC_I_CONTINUE_NEEDED) {
      continue;
    }
    std::fprintf(stderr, "tls server handshake status=0x%08lx\n", static_cast<unsigned long>(st));
    tls_close(s);
    return nullptr;
  }
}

TlsSession* complete_handshake_client(SOCKET sock, CredHandle* cred, bool insecure,
                                      const std::string& expected_fp) {
  auto* s = new TlsSession();
  s->sock = sock;
  s->cred = *cred;
  s->cred_valid = true;
  s->server = false;

  DWORD flags_in = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                   ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_USE_SUPPLIED_CREDS |
                   ISC_REQ_MANUAL_CRED_VALIDATION;
  DWORD flags_out = 0;
  std::vector<char> inbuf;

  // First token (ClientHello)
  {
    SecBuffer out_bufs[1]{};
    out_bufs[0].BufferType = SECBUFFER_TOKEN;
    SecBufferDesc out_desc{SECBUFFER_VERSION, 1, out_bufs};
    TimeStamp expiry{};
    const SECURITY_STATUS st =
        InitializeSecurityContextW(&s->cred, nullptr, const_cast<SEC_WCHAR*>(L"road-desk"),
                                   flags_in, 0, 0, nullptr, 0, &s->ctx, &out_desc, &flags_out,
                                   &expiry);
    s->ctx_valid = true;
    if (FAILED(st) && st != SEC_I_CONTINUE_NEEDED) {
      tls_close(s);
      return nullptr;
    }
    if (send_token(sock, &out_bufs[0]) != SEC_E_OK) {
      tls_close(s);
      return nullptr;
    }
    if (st == SEC_E_OK) {
      // Unusual but handle.
    }
  }

  for (;;) {
    char tmp[kIoBuf];
    const int n = recv_some(sock, tmp, sizeof(tmp));
    if (n <= 0) {
      tls_close(s);
      return nullptr;
    }
    inbuf.insert(inbuf.end(), tmp, tmp + n);

    SecBuffer in_bufs[2]{};
    in_bufs[0].BufferType = SECBUFFER_TOKEN;
    in_bufs[0].pvBuffer = inbuf.data();
    in_bufs[0].cbBuffer = static_cast<ULONG>(inbuf.size());
    in_bufs[1].BufferType = SECBUFFER_EMPTY;
    SecBufferDesc in_desc{SECBUFFER_VERSION, 2, in_bufs};

    SecBuffer out_bufs[1]{};
    out_bufs[0].BufferType = SECBUFFER_TOKEN;
    SecBufferDesc out_desc{SECBUFFER_VERSION, 1, out_bufs};
    TimeStamp expiry{};

    const SECURITY_STATUS st =
        InitializeSecurityContextW(&s->cred, &s->ctx, const_cast<SEC_WCHAR*>(L"road-desk"),
                                   flags_in, 0, 0, &in_desc, 0, nullptr, &out_desc, &flags_out,
                                   &expiry);

    if (out_bufs[0].cbBuffer && out_bufs[0].pvBuffer) {
      if (send_token(sock, &out_bufs[0]) != SEC_E_OK) {
        tls_close(s);
        return nullptr;
      }
    }

    if (st == SEC_E_INCOMPLETE_MESSAGE) {
      continue;
    }

    if (in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer > 0) {
      const size_t extra = in_bufs[1].cbBuffer;
      std::memmove(inbuf.data(), inbuf.data() + (inbuf.size() - extra), extra);
      inbuf.resize(extra);
    } else {
      inbuf.clear();
    }

    if (st == SEC_E_OK) {
      if (!query_sizes(s) || !capture_peer_cert(s)) {
        tls_close(s);
        return nullptr;
      }
      const std::string fp = cert_fingerprint(s->peer_cert);
      if (!insecure) {
        if (expected_fp.empty() || !eq_fingerprint(fp, expected_fp)) {
          tls_close(s);
          return nullptr;
        }
      }
      if (!inbuf.empty()) {
        s->recv_encrypted.insert(s->recv_encrypted.end(), inbuf.begin(), inbuf.end());
      }
      return s;
    }
    if (st == SEC_I_CONTINUE_NEEDED) {
      continue;
    }
    tls_close(s);
    return nullptr;
  }
}

int decrypt_available(TlsSession* s) {
  if (!s->sizes_valid) {
    return -1;
  }
  while (s->decrypted_off >= s->recv_decrypted.size() && !s->recv_encrypted.empty()) {
    s->decrypted_off = 0;
    s->recv_decrypted.clear();

    SecBuffer bufs[4]{};
    bufs[0].BufferType = SECBUFFER_DATA;
    bufs[0].pvBuffer = s->recv_encrypted.data();
    bufs[0].cbBuffer = static_cast<ULONG>(s->recv_encrypted.size());
    bufs[1].BufferType = SECBUFFER_EMPTY;
    bufs[2].BufferType = SECBUFFER_EMPTY;
    bufs[3].BufferType = SECBUFFER_EMPTY;
    SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};

    const SECURITY_STATUS st = DecryptMessage(&s->ctx, &desc, 0, nullptr);
    if (st == SEC_E_INCOMPLETE_MESSAGE) {
      return 0;
    }
    if (st == SEC_I_CONTEXT_EXPIRED || st == SEC_E_CONTEXT_EXPIRED) {
      return -1;
    }
    if (st != SEC_E_OK && st != SEC_I_RENEGOTIATE) {
      return -1;
    }

    for (int i = 0; i < 4; ++i) {
      if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].pvBuffer && bufs[i].cbBuffer) {
        auto* p = static_cast<char*>(bufs[i].pvBuffer);
        s->recv_decrypted.insert(s->recv_decrypted.end(), p, p + bufs[i].cbBuffer);
      }
    }

    size_t consumed = s->recv_encrypted.size();
    for (int i = 0; i < 4; ++i) {
      if (bufs[i].BufferType == SECBUFFER_EXTRA && bufs[i].cbBuffer > 0) {
        consumed = s->recv_encrypted.size() - bufs[i].cbBuffer;
        break;
      }
    }
    if (consumed >= s->recv_encrypted.size()) {
      s->recv_encrypted.clear();
    } else {
      s->recv_encrypted.erase(s->recv_encrypted.begin(),
                              s->recv_encrypted.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
  }
  return static_cast<int>(s->recv_decrypted.size() - s->decrypted_off);
}

DWORD WINAPI bridge_main(LPVOID param) {
  auto* b = static_cast<Bridge*>(param);
  std::vector<char> buf(kIoBuf);
  while (InterlockedCompareExchange(&b->stop, 0, 0) == 0) {
    fd_set rfds;
    FD_ZERO(&rfds);
    SOCKET maxfd = 0;
    if (b->plain != INVALID_SOCKET) {
      FD_SET(b->plain, &rfds);
      if (b->plain > maxfd) {
        maxfd = b->plain;
      }
    }
    if (b->tls && b->tls->sock != INVALID_SOCKET) {
      FD_SET(b->tls->sock, &rfds);
      if (b->tls->sock > maxfd) {
        maxfd = b->tls->sock;
      }
    }
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    const int sel = select(static_cast<int>(maxfd) + 1, &rfds, nullptr, nullptr, &tv);
    if (sel < 0) {
      break;
    }
    if (sel == 0) {
      continue;
    }

    if (b->tls && b->tls->sock != INVALID_SOCKET && FD_ISSET(b->tls->sock, &rfds)) {
      char tmp[kIoBuf];
      const int n = recv(b->tls->sock, tmp, sizeof(tmp), 0);
      if (n <= 0) {
        break;
      }
      b->tls->recv_encrypted.insert(b->tls->recv_encrypted.end(), tmp, tmp + n);
      for (;;) {
        const int avail = decrypt_available(b->tls);
        if (avail < 0) {
          InterlockedExchange(&b->stop, 1);
          break;
        }
        if (avail == 0) {
          break;
        }
        const int chunk =
            (avail > static_cast<int>(buf.size())) ? static_cast<int>(buf.size()) : avail;
        std::memcpy(buf.data(), b->tls->recv_decrypted.data() + b->tls->decrypted_off,
                    static_cast<size_t>(chunk));
        b->tls->decrypted_off += static_cast<size_t>(chunk);
        int sent = 0;
        while (sent < chunk) {
          const int w = send(b->plain, buf.data() + sent, chunk - sent, 0);
          if (w <= 0) {
            InterlockedExchange(&b->stop, 1);
            break;
          }
          sent += w;
        }
        if (InterlockedCompareExchange(&b->stop, 0, 0) != 0) {
          break;
        }
      }
    }

    if (b->plain != INVALID_SOCKET && FD_ISSET(b->plain, &rfds)) {
      const int n = recv(b->plain, buf.data(), static_cast<int>(buf.size()), 0);
      if (n <= 0) {
        break;
      }
      if (tls_write(b->tls, buf.data(), n) != n) {
        break;
      }
    }
  }

  if (b->plain != INVALID_SOCKET) {
    shutdown(b->plain, SD_BOTH);
    closesocket(b->plain);
    b->plain = INVALID_SOCKET;
  }
  if (b->tls) {
    tls_close(b->tls);
    b->tls = nullptr;
  }
  return 0;
}

}  // namespace

bool create_self_signed_credentials(HostCredentials* out) {
  if (!out) {
    return false;
  }
  free_host_credentials(out);

  // PFXImportCertStore may mutate the blob — copy first.
  std::vector<BYTE> pfx_copy(k_mvp_host_pfx, k_mvp_host_pfx + k_mvp_host_pfx_size);
  CRYPT_DATA_BLOB blob{};
  blob.pbData = pfx_copy.data();
  blob.cbData = static_cast<DWORD>(pfx_copy.size());

  wchar_t password[64] = {};
  MultiByteToWideChar(CP_ACP, 0, k_mvp_host_pfx_password, -1, password, 64);

  // Prefer persisted-in-memory key association Schannel can use for server auth.
  HCERTSTORE store = PFXImportCertStore(&blob, password, CRYPT_EXPORTABLE);
  if (!store) {
    return false;
  }

  PCCERT_CONTEXT cert = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                                   0, CERT_FIND_ANY, nullptr, nullptr);
  if (!cert) {
    CertCloseStore(store, 0);
    return false;
  }

  // Duplicate so we can close the temporary store.
  PCCERT_CONTEXT owned = CertDuplicateCertificateContext(cert);
  CertFreeCertificateContext(cert);
  CertCloseStore(store, 0);
  if (!owned) {
    return false;
  }

  DWORD key_spec = 0;
  BOOL caller_free = FALSE;
  HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
  if (!CryptAcquireCertificatePrivateKey(owned, CRYPT_ACQUIRE_ALLOW_NCRYPT_KEY_FLAG, nullptr, &key,
                                         &key_spec, &caller_free)) {
    CertFreeCertificateContext(owned);
    return false;
  }
  if (caller_free) {
    if (key_spec == CERT_NCRYPT_KEY_SPEC) {
      NCryptFreeObject(key);
    } else {
      CryptReleaseContext(key, 0);
    }
  }

  out->cert_context = const_cast<PCERT_CONTEXT>(owned);
  out->fingerprint_sha256_hex = cert_fingerprint(owned);
  if (out->fingerprint_sha256_hex.empty()) {
    out->fingerprint_sha256_hex = k_mvp_host_fingerprint_sha256;
  }
  return true;
}

void free_host_credentials(HostCredentials* creds) {
  if (!creds) {
    return;
  }
  if (creds->cert_context) {
    CertFreeCertificateContext(static_cast<PCCERT_CONTEXT>(creds->cert_context));
    creds->cert_context = nullptr;
  }
  creds->fingerprint_sha256_hex.clear();
}

std::string peer_fingerprint_sha256(TlsSession* session) {
  if (!session || !session->peer_cert) {
    return {};
  }
  return cert_fingerprint(session->peer_cert);
}

SOCKET tcp_listen(int port) {
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    return INVALID_SOCKET;
  }
  BOOL yes = TRUE;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<u_short>(port));
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    const int err = WSAGetLastError();
    closesocket(s);
    WSASetLastError(err);
    return INVALID_SOCKET;
  }
  if (listen(s, 4) == SOCKET_ERROR) {
    const int err = WSAGetLastError();
    closesocket(s);
    WSASetLastError(err);
    return INVALID_SOCKET;
  }
  u_long nonblock = 1;
  ioctlsocket(s, FIONBIO, &nonblock);
  return s;
}

SOCKET tcp_accept(SOCKET listen_sock) {
  SOCKET s = accept(listen_sock, nullptr, nullptr);
  if (s == INVALID_SOCKET) {
    return INVALID_SOCKET;
  }
  BOOL one = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
  u_long block = 0;
  ioctlsocket(s, FIONBIO, &block);
  return s;
}

SOCKET tcp_connect(const char* host, int port) {
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    return INVALID_SOCKET;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<u_short>(port));
  addr.sin_addr.s_addr = inet_addr(host);
  if (addr.sin_addr.s_addr == INADDR_NONE) {
    hostent* he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
      closesocket(s);
      return INVALID_SOCKET;
    }
    std::memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
  }
  if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    closesocket(s);
    return INVALID_SOCKET;
  }
  BOOL one = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
  return s;
}

TlsSession* server_handshake(SOCKET tcp_sock, const HostCredentials& creds) {
  CredHandle cred{};
  if (!init_server_cred(creds, &cred)) {
    closesocket(tcp_sock);
    return nullptr;
  }
  TlsSession* s = complete_handshake_server(tcp_sock, &cred);
  if (!s) {
    FreeCredentialsHandle(&cred);
  }
  return s;
}

TlsSession* client_handshake(SOCKET tcp_sock, bool insecure,
                             const std::string& expected_fingerprint_sha256_hex) {
  CredHandle cred{};
  if (!init_client_cred(&cred)) {
    closesocket(tcp_sock);
    return nullptr;
  }
  TlsSession* s =
      complete_handshake_client(tcp_sock, &cred, insecure, expected_fingerprint_sha256_hex);
  if (!s) {
    FreeCredentialsHandle(&cred);
  }
  return s;
}

int tls_read(TlsSession* session, void* buf, int len) {
  if (!session || !buf || len <= 0) {
    return -1;
  }
  for (;;) {
    const int avail = decrypt_available(session);
    if (avail < 0) {
      return -1;
    }
    if (avail > 0) {
      const int n = (avail < len) ? avail : len;
      std::memcpy(buf, session->recv_decrypted.data() + session->decrypted_off,
                  static_cast<size_t>(n));
      session->decrypted_off += static_cast<size_t>(n);
      return n;
    }
    char tmp[kIoBuf];
    const int r = recv_some(session->sock, tmp, sizeof(tmp));
    if (r <= 0) {
      return r;
    }
    session->recv_encrypted.insert(session->recv_encrypted.end(), tmp, tmp + r);
  }
}

int tls_write(TlsSession* session, const void* buf, int len) {
  if (!session || !buf || len < 0 || !session->sizes_valid) {
    return -1;
  }
  const char* p = static_cast<const char*>(buf);
  int remaining = len;
  while (remaining > 0) {
    const int chunk = (remaining > static_cast<int>(session->sizes.cbMaximumMessage))
                          ? static_cast<int>(session->sizes.cbMaximumMessage)
                          : remaining;
    const size_t total = session->sizes.cbHeader + static_cast<size_t>(chunk) +
                         session->sizes.cbTrailer;
    std::vector<char> packet(total);
    std::memcpy(packet.data() + session->sizes.cbHeader, p, static_cast<size_t>(chunk));

    SecBuffer bufs[4]{};
    bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
    bufs[0].pvBuffer = packet.data();
    bufs[0].cbBuffer = session->sizes.cbHeader;
    bufs[1].BufferType = SECBUFFER_DATA;
    bufs[1].pvBuffer = packet.data() + session->sizes.cbHeader;
    bufs[1].cbBuffer = static_cast<ULONG>(chunk);
    bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
    bufs[2].pvBuffer = packet.data() + session->sizes.cbHeader + chunk;
    bufs[2].cbBuffer = session->sizes.cbTrailer;
    bufs[3].BufferType = SECBUFFER_EMPTY;
    SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};

    const SECURITY_STATUS st = EncryptMessage(&session->ctx, 0, &desc, 0);
    if (st != SEC_E_OK) {
      return -1;
    }
    const int wire = static_cast<int>(bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer);
    if (!send_all(session->sock, packet.data(), wire)) {
      return -1;
    }
    p += chunk;
    remaining -= chunk;
  }
  return len;
}

SOCKET tls_get_socket(TlsSession* session) {
  return session ? session->sock : INVALID_SOCKET;
}

int tls_pending(TlsSession* session) {
  if (!session) {
    return -1;
  }
  return decrypt_available(session);
}

void tls_close(TlsSession* session) {
  if (!session) {
    return;
  }
  if (session->ctx_valid) {
    DeleteSecurityContext(&session->ctx);
    session->ctx_valid = false;
  }
  if (session->cred_valid) {
    FreeCredentialsHandle(&session->cred);
    session->cred_valid = false;
  }
  if (session->peer_cert) {
    CertFreeCertificateContext(session->peer_cert);
    session->peer_cert = nullptr;
  }
  if (session->sock != INVALID_SOCKET) {
    closesocket(session->sock);
    session->sock = INVALID_SOCKET;
  }
  delete session;
}

bool make_socket_pair(SOCKET* a, SOCKET* b) {
  if (!a || !b) {
    return false;
  }
  *a = INVALID_SOCKET;
  *b = INVALID_SOCKET;

  SOCKET lst = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (lst == INVALID_SOCKET) {
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(lst, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    closesocket(lst);
    return false;
  }
  int alen = sizeof(addr);
  if (getsockname(lst, reinterpret_cast<sockaddr*>(&addr), &alen) == SOCKET_ERROR) {
    closesocket(lst);
    return false;
  }
  if (listen(lst, 1) == SOCKET_ERROR) {
    closesocket(lst);
    return false;
  }
  SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (client == INVALID_SOCKET) {
    closesocket(lst);
    return false;
  }
  if (connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    closesocket(client);
    closesocket(lst);
    return false;
  }
  SOCKET server = accept(lst, nullptr, nullptr);
  closesocket(lst);
  if (server == INVALID_SOCKET) {
    closesocket(client);
    return false;
  }
  BOOL one = TRUE;
  setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
  setsockopt(server, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
  *a = client;
  *b = server;
  return true;
}

bool start_bridge(Bridge* bridge, TlsSession* tls, SOCKET plain) {
  if (!bridge || !tls || plain == INVALID_SOCKET) {
    return false;
  }
  bridge->tls = tls;
  bridge->plain = plain;
  bridge->stop = 0;
  bridge->thread = CreateThread(nullptr, 0, bridge_main, bridge, 0, nullptr);
  return bridge->thread != nullptr;
}

void stop_bridge(Bridge* bridge) {
  if (!bridge) {
    return;
  }
  InterlockedExchange(&bridge->stop, 1);
  if (bridge->plain != INVALID_SOCKET) {
    shutdown(bridge->plain, SD_BOTH);
  }
  if (bridge->tls && bridge->tls->sock != INVALID_SOCKET) {
    shutdown(bridge->tls->sock, SD_BOTH);
  }
  if (bridge->thread) {
    WaitForSingleObject(bridge->thread, 10000);
    CloseHandle(bridge->thread);
    bridge->thread = nullptr;
  }
  // bridge_main closes tls/plain; if thread never ran, clean up here.
  if (bridge->plain != INVALID_SOCKET) {
    closesocket(bridge->plain);
    bridge->plain = INVALID_SOCKET;
  }
  if (bridge->tls) {
    tls_close(bridge->tls);
    bridge->tls = nullptr;
  }
}

bool parse_host_port(const std::string& host_port, std::string* host, int* port) {
  if (!host || !port) {
    return false;
  }
  const auto colon = host_port.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= host_port.size()) {
    return false;
  }
  *host = host_port.substr(0, colon);
  *port = std::atoi(host_port.c_str() + colon + 1);
  if (*port <= 0 || *port > 65535) {
    return false;
  }
  // LibVNC treats ports < 5900 as display numbers; we use absolute TCP ports.
  return true;
}

}  // namespace road_desk::media::tls
