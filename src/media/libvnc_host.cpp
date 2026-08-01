// LibVNC host adapter — GPL, internal MVP only (ADR-0002).

#include "media_plane.h"
#include "tls_schannel.h"
#include "win_input.h"

#include "session_mutex.h"

#include <rfb/rfb.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace road_desk::media {
namespace {

constexpr int kBlock = 32;

struct ClientBridgeData {
  tls::Bridge bridge;
};

struct HostState {
  rfbScreenInfoPtr screen = nullptr;
  std::vector<char> fb;
  std::vector<char> prev;
  int width = 0;
  int height = 0;
  int clients = 0;

  HDC screen_dc = nullptr;
  HDC mem_dc = nullptr;
  HBITMAP dib = nullptr;
  HGDIOBJ old_bmp = nullptr;
  void* dib_bits = nullptr;

  std::string password;
  char* password_list[2] = {nullptr, nullptr};
  std::string desktop_name;

  bool require_tls = true;
  session::SessionMutex* session_mutex = nullptr;
  tls::HostCredentials tls_creds{};
  SOCKET tls_listen = INVALID_SOCKET;
};

HostState* g_host = nullptr;

void on_ptr(int button_mask, int x, int y, rfbClientPtr /*cl*/) {
  inject_pointer(button_mask, x, y);
}

void on_kbd(rfbBool down, rfbKeySym keysym, rfbClientPtr /*cl*/) {
  inject_key(down, keysym);
}

void on_client_gone(rfbClientPtr cl) {
  release_injected_modifiers();
  if (cl && cl->clientData) {
    auto* data = static_cast<ClientBridgeData*>(cl->clientData);
    tls::stop_bridge(&data->bridge);
    delete data;
    cl->clientData = nullptr;
  }
  if (g_host && g_host->clients > 0) {
    g_host->clients--;
  }
  if (g_host && g_host->session_mutex) {
    g_host->session_mutex->release();
  }
}

enum rfbNewClientAction on_new_client(rfbClientPtr cl) {
  if (!g_host) {
    return RFB_CLIENT_REFUSE;
  }
  if (g_host->clients >= 1) {
    rfbLog("media host: refuse second client (MVP single session)\n");
    return RFB_CLIENT_REFUSE;
  }
  if (g_host->session_mutex && !g_host->session_mutex->try_acquire()) {
    rfbLog("media host: SessionMutex busy — refuse client\n");
    return RFB_CLIENT_REFUSE;
  }
  g_host->clients++;
  cl->clientGoneHook = on_client_gone;
  rfbLog("media host: client accepted\n");
  return RFB_CLIENT_ACCEPT;
}

void apply_server_format(rfbScreenInfoPtr screen) {
  screen->serverFormat.bitsPerPixel = 32;
  screen->serverFormat.depth = 24;
  screen->serverFormat.trueColour = TRUE;
  screen->serverFormat.bigEndian = FALSE;
  screen->serverFormat.redMax = 255;
  screen->serverFormat.greenMax = 255;
  screen->serverFormat.blueMax = 255;
  screen->serverFormat.redShift = 16;
  screen->serverFormat.greenShift = 8;
  screen->serverFormat.blueShift = 0;
}

void release_capture(HostState* st) {
  if (st->mem_dc && st->old_bmp) {
    SelectObject(st->mem_dc, st->old_bmp);
    st->old_bmp = nullptr;
  }
  if (st->dib) {
    DeleteObject(st->dib);
    st->dib = nullptr;
    st->dib_bits = nullptr;
  }
  if (st->mem_dc) {
    DeleteDC(st->mem_dc);
    st->mem_dc = nullptr;
  }
  if (st->screen_dc) {
    ReleaseDC(nullptr, st->screen_dc);
    st->screen_dc = nullptr;
  }
}

bool ensure_capture_buffers(HostState* st, int w, int h) {
  if (w == st->width && h == st->height && st->dib && !st->fb.empty()) {
    return true;
  }

  release_capture(st);
  st->width = w;
  st->height = h;
  st->fb.assign(static_cast<size_t>(w) * h * 4, 0);
  st->prev = st->fb;

  st->screen_dc = GetDC(nullptr);
  st->mem_dc = CreateCompatibleDC(st->screen_dc);

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  st->dib = CreateDIBSection(st->mem_dc, &bmi, DIB_RGB_COLORS, &st->dib_bits, nullptr, 0);
  if (!st->dib || !st->dib_bits) {
    release_capture(st);
    return false;
  }
  st->old_bmp = SelectObject(st->mem_dc, st->dib);

  if (st->screen) {
    rfbNewFramebuffer(st->screen, st->fb.data(), w, h, 8, 3, 4);
    apply_server_format(st->screen);
  }
  return true;
}

bool capture_desktop(HostState* st) {
  const int w = GetSystemMetrics(SM_CXSCREEN);
  const int h = GetSystemMetrics(SM_CYSCREEN);
  if (w <= 0 || h <= 0) {
    return false;
  }
  if (!ensure_capture_buffers(st, w, h)) {
    return false;
  }

  BitBlt(st->mem_dc, 0, 0, w, h, st->screen_dc, 0, 0, SRCCOPY | CAPTUREBLT);
  std::memcpy(st->fb.data(), st->dib_bits, static_cast<size_t>(w) * h * 4);
  return true;
}

void copy_rect_to_prev(HostState* st, int x0, int y0, int x1, int y1) {
  const int stride = st->width * 4;
  const size_t row_bytes = static_cast<size_t>(x1 - x0) * 4;
  for (int y = y0; y < y1; ++y) {
    const size_t off = static_cast<size_t>(y) * stride + static_cast<size_t>(x0) * 4;
    std::memcpy(st->prev.data() + off, st->fb.data() + off, row_bytes);
  }
}

void mark_dirty_blocks(HostState* st) {
  if (!st->screen || st->fb.empty() || st->prev.size() != st->fb.size()) {
    return;
  }
  const int stride = st->width * 4;
  for (int y = 0; y < st->height; y += kBlock) {
    const int yh = (y + kBlock < st->height) ? (y + kBlock) : st->height;
    int run_x0 = -1;
    int run_x1 = -1;
    for (int x = 0; x < st->width; x += kBlock) {
      const int xw = (x + kBlock < st->width) ? (x + kBlock) : st->width;
      bool dirty = false;
      for (int yy = y; yy < yh && !dirty; ++yy) {
        const char* a = st->fb.data() + yy * stride + x * 4;
        const char* b = st->prev.data() + yy * stride + x * 4;
        if (std::memcmp(a, b, static_cast<size_t>(xw - x) * 4) != 0) {
          dirty = true;
        }
      }
      if (dirty) {
        if (run_x0 < 0) {
          run_x0 = x;
        }
        run_x1 = xw;
      } else if (run_x0 >= 0) {
        rfbMarkRectAsModified(st->screen, run_x0, y, run_x1, yh);
        copy_rect_to_prev(st, run_x0, y, run_x1, yh);
        run_x0 = -1;
        run_x1 = -1;
      }
    }
    if (run_x0 >= 0) {
      rfbMarkRectAsModified(st->screen, run_x0, y, run_x1, yh);
      copy_rect_to_prev(st, run_x0, y, run_x1, yh);
    }
  }
}

bool listen_ok_plain(rfbScreenInfoPtr screen) {
  if (!screen || screen->listenSock == RFB_INVALID_SOCKET) {
    return false;
  }
  if (!FD_ISSET(screen->listenSock, &screen->allFds)) {
    return false;
  }

  fd_set fds;
  memcpy(&fds, &screen->allFds, sizeof(fds));
  timeval tv{};
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  const int n = select(static_cast<int>(screen->maxFd) + 1, &fds, nullptr, nullptr, &tv);
  if (n < 0) {
    return false;
  }
  return true;
}

void poll_tls_accept(HostState* st) {
  if (!st || st->tls_listen == INVALID_SOCKET || !st->screen) {
    return;
  }
  SOCKET tcp = tls::tcp_accept(st->tls_listen);
  if (tcp == INVALID_SOCKET) {
    return;
  }

  if (st->clients >= 1 || (st->session_mutex && st->session_mutex->held())) {
    rfbLog("media host: TLS accept refused (session busy)\n");
    closesocket(tcp);
    return;
  }

  tls::TlsSession* tls_sess = tls::server_handshake(tcp, st->tls_creds);
  if (!tls_sess) {
    rfbLog("media host: TLS handshake failed\n");
    return;
  }

  SOCKET libvnc_end = INVALID_SOCKET;
  SOCKET bridge_end = INVALID_SOCKET;
  if (!tls::make_socket_pair(&libvnc_end, &bridge_end)) {
    rfbLog("media host: socket pair failed\n");
    tls::tls_close(tls_sess);
    return;
  }

  auto* data = new ClientBridgeData();
  if (!tls::start_bridge(&data->bridge, tls_sess, bridge_end)) {
    rfbLog("media host: TLS bridge start failed\n");
    delete data;
    closesocket(libvnc_end);
    closesocket(bridge_end);
    tls::tls_close(tls_sess);
    return;
  }
  // bridge owns tls_sess + bridge_end

  rfbClientPtr cl = rfbNewClient(st->screen, libvnc_end);
  if (!cl) {
    // REFUSE / failure: LibVNC closed libvnc_end; tear down TLS bridge.
    rfbLog("media host: rfbNewClient refused or failed\n");
    tls::stop_bridge(&data->bridge);
    delete data;
    return;
  }
  cl->clientData = data;
}

}  // namespace

struct MediaPlane::Impl {
  HostState state;
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> running{false};
  std::atomic<int> bound_port{0};
};

MediaPlane::MediaPlane() : impl_(new Impl) {}

MediaPlane::~MediaPlane() {
  request_stop();
  if (impl_) {
    if (impl_->state.tls_listen != INVALID_SOCKET) {
      closesocket(impl_->state.tls_listen);
      impl_->state.tls_listen = INVALID_SOCKET;
    }
    tls::free_host_credentials(&impl_->state.tls_creds);
    if (impl_->state.screen) {
      rfbShutdownServer(impl_->state.screen, TRUE);
      rfbScreenCleanup(impl_->state.screen);
      impl_->state.screen = nullptr;
      release_capture(&impl_->state);
      if (g_host == &impl_->state) {
        g_host = nullptr;
      }
    }
  }
  delete impl_;
  impl_ = nullptr;
}

bool MediaPlane::listen(const MediaPlaneConfig& config) {
  if (!impl_ || impl_->running.load() || impl_->state.screen) {
    return false;
  }

  SetProcessDPIAware();

  HostState* st = &impl_->state;
  g_host = st;
  st->password = config.password;
  st->password_list[0] = st->password.empty() ? nullptr : st->password.data();
  st->password_list[1] = nullptr;
  st->desktop_name = config.desktop_name;
  st->clients = 0;
  st->require_tls = config.require_tls;
  st->session_mutex = config.session_mutex;
  impl_->stop_requested = false;

  auto fail = [this, st]() {
    if (st->tls_listen != INVALID_SOCKET) {
      closesocket(st->tls_listen);
      st->tls_listen = INVALID_SOCKET;
    }
    tls::free_host_credentials(&st->tls_creds);
    if (st->screen) {
      rfbShutdownServer(st->screen, TRUE);
      rfbScreenCleanup(st->screen);
      st->screen = nullptr;
    }
    release_capture(st);
    if (g_host == st) {
      g_host = nullptr;
    }
    impl_->running = false;
    impl_->bound_port = 0;
  };

  if (!capture_desktop(st)) {
    fail();
    return false;
  }

  // TLS mode: LibVNC must not open a cleartext listen port (port<=0 skips listen).
  const int libvnc_port = st->require_tls ? 0 : config.listen_port;
  const std::string port_str = std::to_string(libvnc_port);
  char arg0[] = "host-agent";
  char arg1[] = "-rfbport";
  std::vector<char> port_arg(port_str.begin(), port_str.end());
  port_arg.push_back('\0');
  char* argv[] = {arg0, arg1, port_arg.data(), nullptr};
  int argc = 3;
  st->screen = rfbGetScreen(&argc, argv, st->width, st->height, 8, 3, 4);
  if (!st->screen) {
    fail();
    return false;
  }

  st->screen->frameBuffer = st->fb.data();
  st->screen->desktopName = st->desktop_name.data();
  st->screen->alwaysShared = TRUE;
  st->screen->ptrAddEvent = on_ptr;
  st->screen->kbdAddEvent = on_kbd;
  st->screen->newClientHook = on_new_client;
  st->screen->authPasswdData = reinterpret_cast<void*>(st->password_list);
  st->screen->passwordCheck = rfbCheckPasswordByList;
  apply_server_format(st->screen);
  st->screen->httpDir = nullptr;
  st->screen->httpPort = 0;
  st->screen->deferUpdateTime = 1;
  st->screen->port = libvnc_port;
  st->screen->autoPort = FALSE;
  st->screen->ipv6port = 0;

  rfbInitServer(st->screen);

  if (st->require_tls) {
    if (!tls::create_self_signed_credentials(&st->tls_creds)) {
      rfbLog("media host: self-signed TLS cert creation failed\n");
      fail();
      return false;
    }
    st->tls_listen = tls::tcp_listen(config.listen_port);
    if (st->tls_listen == INVALID_SOCKET) {
      rfbLog("media host: TLS listen failed\n");
      fail();
      return false;
    }
    // Fingerprint is logged once by host-agent (log_line), not here.
    impl_->bound_port = config.listen_port;
  } else {
    if (!listen_ok_plain(st->screen)) {
      fail();
      return false;
    }
    impl_->bound_port = st->screen->port;
  }

  impl_->running = true;
  return true;
}

void MediaPlane::serve() {
  if (!impl_ || !impl_->state.screen) {
    return;
  }
  HostState* st = &impl_->state;
  while (!impl_->stop_requested.load() && st->screen && rfbIsActive(st->screen)) {
    if (st->require_tls) {
      if (st->tls_listen == INVALID_SOCKET) {
        break;
      }
      poll_tls_accept(st);
      if (st->clients == 0) {
        // No LibVNC listenSock in TLS mode — avoid empty select spam (rfbCheckFds).
        Sleep(50);
        continue;
      }
    } else if (st->screen->listenSock == RFB_INVALID_SOCKET) {
      break;
    }

    if (st->clients > 0) {
      if (rfbProcessEvents(st->screen, 2000)) {
        rfbProcessEvents(st->screen, 5000);
        continue;
      }
      if (capture_desktop(st)) {
        st->screen->frameBuffer = st->fb.data();
        mark_dirty_blocks(st);
      }
      rfbProcessEvents(st->screen, 5000);
    } else {
      rfbProcessEvents(st->screen, 50000);
    }
  }

  if (st->tls_listen != INVALID_SOCKET) {
    closesocket(st->tls_listen);
    st->tls_listen = INVALID_SOCKET;
  }
  tls::free_host_credentials(&st->tls_creds);

  if (st->screen) {
    rfbShutdownServer(st->screen, TRUE);
    rfbScreenCleanup(st->screen);
    st->screen = nullptr;
  }
  release_capture(st);
  release_injected_modifiers();
  if (g_host == st) {
    g_host = nullptr;
  }
  impl_->running = false;
  impl_->bound_port = 0;
}

void MediaPlane::request_stop() {
  if (!impl_) {
    return;
  }
  impl_->stop_requested = true;
  if (impl_->state.tls_listen != INVALID_SOCKET) {
    closesocket(impl_->state.tls_listen);
    impl_->state.tls_listen = INVALID_SOCKET;
  }
  if (impl_->state.screen) {
    rfbShutdownServer(impl_->state.screen, TRUE);
  }
}

bool MediaPlane::running() const {
  return impl_ && impl_->running.load();
}

int MediaPlane::bound_port() const {
  return impl_ ? impl_->bound_port.load() : 0;
}

std::string MediaPlane::tls_fingerprint_sha256() const {
  if (!impl_) {
    return {};
  }
  return impl_->state.tls_creds.fingerprint_sha256_hex;
}

}  // namespace road_desk::media
