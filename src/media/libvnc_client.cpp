// LibVNC client adapter — GPL, internal MVP only (ADR-0002).

#include "media_plane.h"
#include "tls_schannel.h"

#include <rfb/rfbclient.h>
#include <rfb/keysym.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace road_desk::media {
namespace {

rfbKeySym vk_to_keysym(unsigned vk) {
  switch (vk) {
    case VK_BACK:
      return XK_BackSpace;
    case VK_TAB:
      return XK_Tab;
    case VK_RETURN:
      return XK_Return;
    case VK_ESCAPE:
      return XK_Escape;
    case VK_DELETE:
      return XK_Delete;
    case VK_LEFT:
      return XK_Left;
    case VK_UP:
      return XK_Up;
    case VK_RIGHT:
      return XK_Right;
    case VK_DOWN:
      return XK_Down;
    case VK_HOME:
      return XK_Home;
    case VK_END:
      return XK_End;
    case VK_PRIOR:
      return XK_Page_Up;
    case VK_NEXT:
      return XK_Page_Down;
    case VK_INSERT:
      return XK_Insert;
    case VK_LSHIFT:
    case VK_SHIFT:
      return XK_Shift_L;
    case VK_RSHIFT:
      return XK_Shift_R;
    case VK_LCONTROL:
    case VK_CONTROL:
      return XK_Control_L;
    case VK_RCONTROL:
      return XK_Control_R;
    case VK_LMENU:
    case VK_MENU:
      return XK_Alt_L;
    case VK_RMENU:
      return XK_Alt_R;
    case VK_LWIN:
      return XK_Super_L;
    case VK_RWIN:
      return XK_Super_R;
    case VK_F1:
      return XK_F1;
    case VK_F2:
      return XK_F2;
    case VK_F3:
      return XK_F3;
    case VK_F4:
      return XK_F4;
    case VK_F5:
      return XK_F5;
    case VK_F6:
      return XK_F6;
    case VK_F7:
      return XK_F7;
    case VK_F8:
      return XK_F8;
    case VK_F9:
      return XK_F9;
    case VK_F10:
      return XK_F10;
    case VK_F11:
      return XK_F11;
    case VK_F12:
      return XK_F12;
    case VK_SPACE:
      return XK_space;
    default:
      break;
  }
  if (vk >= 0x30 && vk <= 0x39) {
    return static_cast<rfbKeySym>(vk);
  }
  if (vk >= 0x41 && vk <= 0x5a) {
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
    const bool upper = shift ^ caps;
    return static_cast<rfbKeySym>(upper ? vk : (vk + 32));
  }
  return 0;
}

void convert_rect_to_bgra(std::vector<uint8_t>& bgra, const uint8_t* fb, int fb_w, int fb_h,
                          int x, int y, int w, int h, int rshift, int gshift, int bshift) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > fb_w) {
    w = fb_w - x;
  }
  if (y + h > fb_h) {
    h = fb_h - y;
  }
  if (w <= 0 || h <= 0 || bgra.size() < static_cast<size_t>(fb_w) * fb_h * 4) {
    return;
  }
  const auto* src = reinterpret_cast<const uint32_t*>(fb);
  for (int row = 0; row < h; ++row) {
    const int yy = y + row;
    for (int col = 0; col < w; ++col) {
      const int xx = x + col;
      const uint32_t p = src[yy * fb_w + xx];
      const size_t di = (static_cast<size_t>(yy) * fb_w + xx) * 4;
      bgra[di + 0] = static_cast<uint8_t>((p >> bshift) & 0xff);
      bgra[di + 1] = static_cast<uint8_t>((p >> gshift) & 0xff);
      bgra[di + 2] = static_cast<uint8_t>((p >> rshift) & 0xff);
      bgra[di + 3] = 0;
    }
  }
}

struct ClientState {
  MediaClientConfig cfg;
  rfbClient* client = nullptr;
  HANDLE thread = nullptr;
  std::atomic<bool> stop{false};
  std::atomic<bool> connected{false};

  mutable CRITICAL_SECTION fb_cs;
  bool fb_cs_ready = false;
  std::vector<uint8_t> bgra;
  int fb_w = 0;
  int fb_h = 0;
  int rshift = 0;
  int gshift = 8;
  int bshift = 16;

  std::string password_storage;
  tls::Bridge tls_bridge{};
};

ClientState* g_client_state = nullptr;

void init_fb_cs(ClientState* st) {
  if (!st->fb_cs_ready) {
    InitializeCriticalSection(&st->fb_cs);
    st->fb_cs_ready = true;
  }
}

void destroy_fb_cs(ClientState* st) {
  if (st->fb_cs_ready) {
    DeleteCriticalSection(&st->fb_cs);
    st->fb_cs_ready = false;
  }
}

char* get_password(rfbClient* /*client*/) {
  if (!g_client_state) {
    return nullptr;
  }
  return _strdup(g_client_state->password_storage.c_str());
}

rfbBool on_malloc_framebuffer(rfbClient* client) {
  free(client->frameBuffer);
  client->frameBuffer = nullptr;
  const size_t bytes =
      static_cast<size_t>(client->width) * client->height * (client->format.bitsPerPixel / 8);
  client->frameBuffer = static_cast<uint8_t*>(malloc(bytes));
  if (g_client_state && g_client_state->cfg.notify_hwnd) {
    PostMessageW(g_client_state->cfg.notify_hwnd, g_client_state->cfg.resize_msg,
                 static_cast<WPARAM>(client->width), static_cast<LPARAM>(client->height));
  }
  return client->frameBuffer != nullptr;
}

void on_update(rfbClient* client, int x, int y, int w, int h) {
  if (!g_client_state) {
    return;
  }
  auto* st = g_client_state;
  EnterCriticalSection(&st->fb_cs);
  st->fb_w = client->width;
  st->fb_h = client->height;
  const rfbPixelFormat& pf = client->format;
  st->rshift = pf.redShift;
  st->gshift = pf.greenShift;
  st->bshift = pf.blueShift;
  const size_t bytes = static_cast<size_t>(st->fb_w) * st->fb_h * 4;
  if (st->bgra.size() != bytes) {
    std::vector<uint8_t> next(bytes, 0);
    st->bgra.swap(next);
    if (client->frameBuffer) {
      convert_rect_to_bgra(st->bgra, client->frameBuffer, st->fb_w, st->fb_h, 0, 0, st->fb_w,
                           st->fb_h, st->rshift, st->gshift, st->bshift);
    }
  } else if (client->frameBuffer) {
    if (w <= 0 || h <= 0 || x >= st->fb_w || y >= st->fb_h) {
      convert_rect_to_bgra(st->bgra, client->frameBuffer, st->fb_w, st->fb_h, 0, 0, st->fb_w,
                           st->fb_h, st->rshift, st->gshift, st->bshift);
    } else {
      convert_rect_to_bgra(st->bgra, client->frameBuffer, st->fb_w, st->fb_h, x, y, w, h,
                           st->rshift, st->gshift, st->bshift);
    }
  }
  LeaveCriticalSection(&st->fb_cs);
  if (st->cfg.notify_hwnd) {
    // Full invalidate only — partial SetDIBits/blt tore (花屏), same lesson as spike.
    PostMessageW(st->cfg.notify_hwnd, st->cfg.resize_msg, static_cast<WPARAM>(client->width),
                 static_cast<LPARAM>(client->height));
    InvalidateRect(st->cfg.notify_hwnd, nullptr, FALSE);
  }
}

bool init_client_over_socket(rfbClient* client, SOCKET sock) {
  // Skip TCP connect; RFB runs on an already-connected (usually bridged) socket.
  client->listenSpecified = TRUE;
  client->sock = sock;
  if (!InitialiseRFBConnection(client)) {
    return false;
  }
  client->width = client->si.framebufferWidth;
  client->height = client->si.framebufferHeight;
  if (!client->MallocFrameBuffer(client)) {
    return false;
  }
  if (!SetFormatAndEncodings(client)) {
    return false;
  }
  if (client->updateRect.x < 0) {
    client->updateRect.x = client->updateRect.y = 0;
    client->updateRect.w = client->width;
    client->updateRect.h = client->height;
  }
  return SendFramebufferUpdateRequest(client, client->updateRect.x, client->updateRect.y,
                                      client->updateRect.w, client->updateRect.h, FALSE);
}

DWORD WINAPI client_thread_main(LPVOID param) {
  auto* st = static_cast<ClientState*>(param);
  rfbClient* client = rfbGetClient(8, 3, 4);
  if (!client) {
    return 1;
  }
  client->MallocFrameBuffer = on_malloc_framebuffer;
  client->GotFrameBufferUpdate = on_update;
  client->GetPassword = get_password;
  client->canHandleNewFBSize = FALSE;
  // Match spike: hextile first. Preferring raw made drag worse (huge rects backlog → trails).
  // No copyrect — incomplete move handling → 花屏.
  client->appData.encodingsString = const_cast<char*>("hextile ultra raw");
  client->appData.compressLevel = 1;
  client->appData.qualityLevel = 6;

  bool ok = false;
  bool client_owned = true;  // false after rfbInitClient failure (it cleans up)
  if (st->cfg.require_tls) {
    std::string host;
    int port = 0;
    if (!tls::parse_host_port(st->cfg.host_port, &host, &port)) {
      rfbClientLog("media client: bad host:port\n");
    } else {
      SOCKET tcp = tls::tcp_connect(host.c_str(), port);
      if (tcp == INVALID_SOCKET) {
        rfbClientLog("media client: TCP connect failed\n");
      } else {
        tls::TlsSession* tls_sess =
            tls::client_handshake(tcp, st->cfg.tls_insecure, st->cfg.tls_fingerprint_sha256);
        if (!tls_sess) {
          rfbClientLog("media client: TLS handshake/fingerprint failed\n");
        } else {
          SOCKET libvnc_end = INVALID_SOCKET;
          SOCKET bridge_end = INVALID_SOCKET;
          if (!tls::make_socket_pair(&libvnc_end, &bridge_end)) {
            rfbClientLog("media client: socket pair failed\n");
            tls::tls_close(tls_sess);
          } else if (!tls::start_bridge(&st->tls_bridge, tls_sess, bridge_end)) {
            rfbClientLog("media client: TLS bridge failed\n");
            closesocket(libvnc_end);
            closesocket(bridge_end);
            tls::tls_close(tls_sess);
          } else {
            ok = init_client_over_socket(client, libvnc_end);
            if (!ok) {
              tls::stop_bridge(&st->tls_bridge);
            }
          }
        }
      }
    }
  } else {
    std::string host = st->cfg.host_port;
    char* argv_storage[] = {const_cast<char*>("viewer"), host.data(), nullptr};
    int argc = 2;
    char** argv = argv_storage;
    ok = rfbInitClient(client, &argc, argv) ? true : false;
    if (!ok) {
      client_owned = false;
    }
  }

  if (!ok) {
    if (client_owned) {
      rfbClientCleanup(client);
    }
    tls::stop_bridge(&st->tls_bridge);
    if (g_client_state == st) {
      g_client_state = nullptr;
    }
    if (st->cfg.notify_hwnd) {
      PostMessageW(st->cfg.notify_hwnd, WM_CLOSE, 0, 0);
    }
    return 1;
  }

  st->client = client;
  st->connected = true;

  while (!st->stop.load()) {
    const int n = WaitForMessage(client, 5000);
    if (n < 0) {
      break;
    }
    if (n) {
      if (!HandleRFBServerMessage(client)) {
        break;
      }
    }
  }

  st->connected = false;
  st->client = nullptr;
  rfbClientCleanup(client);
  tls::stop_bridge(&st->tls_bridge);
  if (g_client_state == st) {
    g_client_state = nullptr;
  }
  if (st->cfg.notify_hwnd) {
    PostMessageW(st->cfg.notify_hwnd, WM_CLOSE, 0, 0);
  }
  return 0;
}

}  // namespace

struct MediaClient::Impl {
  ClientState state;
};

MediaClient::MediaClient() : impl_(new Impl) {
  init_fb_cs(&impl_->state);
}

MediaClient::~MediaClient() {
  stop();
  if (impl_) {
    destroy_fb_cs(&impl_->state);
  }
  delete impl_;
  impl_ = nullptr;
}

bool MediaClient::start(const MediaClientConfig& config) {
  if (!impl_ || impl_->state.connected.load() || impl_->state.thread) {
    return false;
  }
  ClientState* st = &impl_->state;
  st->cfg = config;
  st->password_storage = config.password;
  st->stop = false;
  g_client_state = st;

  // Win32 CreateThread — same path as spike_viewer (avoid std::thread on Win7).
  st->thread = CreateThread(nullptr, 0, client_thread_main, st, 0, nullptr);
  return st->thread != nullptr;
}

void MediaClient::stop() {
  if (!impl_) {
    return;
  }
  ClientState* st = &impl_->state;
  st->stop = true;
  tls::stop_bridge(&st->tls_bridge);
  if (st->thread) {
    WaitForSingleObject(st->thread, 15000);
    CloseHandle(st->thread);
    st->thread = nullptr;
  }
  st->connected = false;
  st->client = nullptr;
  if (g_client_state == st) {
    g_client_state = nullptr;
  }
}

bool MediaClient::connected() const {
  return impl_ && impl_->state.connected.load();
}

bool MediaClient::copy_frame_bgra(std::vector<uint8_t>& out, int& width, int& height) const {
  if (!impl_) {
    return false;
  }
  EnterCriticalSection(&impl_->state.fb_cs);
  width = impl_->state.fb_w;
  height = impl_->state.fb_h;
  out = impl_->state.bgra;
  LeaveCriticalSection(&impl_->state.fb_cs);
  return width > 0 && height > 0 && !out.empty();
}

void MediaClient::send_pointer(int button_mask, int x, int y) {
  if (!impl_ || !impl_->state.client) {
    return;
  }
  SendPointerEvent(impl_->state.client, x, y, button_mask);
}

bool MediaClient::send_vk(unsigned vk, bool down) {
  if (!impl_ || !impl_->state.client) {
    return false;
  }
  const rfbKeySym ks = vk_to_keysym(vk);
  if (ks == 0) {
    return false;
  }
  SendKeyEvent(impl_->state.client, ks, down ? TRUE : FALSE);
  return true;
}

void MediaClient::release_modifiers() {
  if (!impl_ || !impl_->state.client) {
    return;
  }
  SendKeyEvent(impl_->state.client, XK_Shift_L, FALSE);
  SendKeyEvent(impl_->state.client, XK_Control_L, FALSE);
  SendKeyEvent(impl_->state.client, XK_Alt_L, FALSE);
  SendKeyEvent(impl_->state.client, XK_Super_L, FALSE);
}

}  // namespace road_desk::media
