// Media-plane spike Host: LibVNCServer + GDI primary-display capture + SendInput.
// GPL candidate for internal validation only — not a delivery artifact.

#include "win_input.h"

#include <rfb/rfb.h>  // includes winsock2.h on Windows

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int kBlock = 32;  // tighter dirty rects while dragging windows
constexpr char kPassword[] = "spike";
char* kPasswordList[] = {const_cast<char*>(kPassword), nullptr};

rfbScreenInfoPtr g_screen = nullptr;
std::vector<char> g_fb;
std::vector<char> g_prev;
int g_width = 0;
int g_height = 0;
int g_clients = 0;

// Persistent GDI capture objects (create once per resolution).
HDC g_screen_dc = nullptr;
HDC g_mem_dc = nullptr;
HBITMAP g_dib = nullptr;
HGDIOBJ g_old_bmp = nullptr;
void* g_dib_bits = nullptr;

void on_ptr(int button_mask, int x, int y, rfbClientPtr /*cl*/) {
  spike_inject_pointer(button_mask, x, y);
}

void on_kbd(rfbBool down, rfbKeySym keysym, rfbClientPtr /*cl*/) {
  spike_inject_key(down, keysym);
}

void on_client_gone(rfbClientPtr /*cl*/) {
  spike_release_modifiers();
  if (g_clients > 0) {
    g_clients--;
  }
}

enum rfbNewClientAction on_new_client(rfbClientPtr cl) {
  if (g_clients >= 1) {
    rfbLog("spike_host: refuse second client (MVP single session)\n");
    return RFB_CLIENT_REFUSE;
  }
  g_clients++;
  cl->clientGoneHook = on_client_gone;
  rfbLog("spike_host: client accepted\n");
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

void release_capture() {
  if (g_mem_dc && g_old_bmp) {
    SelectObject(g_mem_dc, g_old_bmp);
    g_old_bmp = nullptr;
  }
  if (g_dib) {
    DeleteObject(g_dib);
    g_dib = nullptr;
    g_dib_bits = nullptr;
  }
  if (g_mem_dc) {
    DeleteDC(g_mem_dc);
    g_mem_dc = nullptr;
  }
  if (g_screen_dc) {
    ReleaseDC(nullptr, g_screen_dc);
    g_screen_dc = nullptr;
  }
}

bool ensure_capture_buffers(int w, int h) {
  if (w == g_width && h == g_height && g_dib && !g_fb.empty()) {
    return true;
  }

  release_capture();
  g_width = w;
  g_height = h;
  g_fb.assign(static_cast<size_t>(w) * h * 4, 0);
  g_prev = g_fb;

  g_screen_dc = GetDC(nullptr);
  g_mem_dc = CreateCompatibleDC(g_screen_dc);

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  g_dib = CreateDIBSection(g_mem_dc, &bmi, DIB_RGB_COLORS, &g_dib_bits, nullptr, 0);
  if (!g_dib || !g_dib_bits) {
    release_capture();
    return false;
  }
  g_old_bmp = SelectObject(g_mem_dc, g_dib);

  if (g_screen) {
    rfbNewFramebuffer(g_screen, g_fb.data(), w, h, 8, 3, 4);
    apply_server_format(g_screen);
  }
  return true;
}

bool capture_desktop() {
  const int w = GetSystemMetrics(SM_CXSCREEN);
  const int h = GetSystemMetrics(SM_CYSCREEN);
  if (w <= 0 || h <= 0) {
    return false;
  }
  if (!ensure_capture_buffers(w, h)) {
    return false;
  }

  BitBlt(g_mem_dc, 0, 0, w, h, g_screen_dc, 0, 0, SRCCOPY | CAPTUREBLT);
  // DIB section is B,G,R,X — matches serverFormat shifts (r16 g8 b0).
  std::memcpy(g_fb.data(), g_dib_bits, static_cast<size_t>(w) * h * 4);
  return true;
}

void mark_dirty_blocks() {
  if (!g_screen || g_fb.empty()) {
    return;
  }
  const int stride = g_width * 4;
  // Coalesce horizontal runs of dirty blocks → fewer RFB rectangles while dragging.
  for (int y = 0; y < g_height; y += kBlock) {
    const int yh = (y + kBlock < g_height) ? (y + kBlock) : g_height;
    int run_x0 = -1;
    int run_x1 = -1;
    for (int x = 0; x < g_width; x += kBlock) {
      const int xw = (x + kBlock < g_width) ? (x + kBlock) : g_width;
      bool dirty = false;
      for (int yy = y; yy < yh && !dirty; ++yy) {
        const char* a = g_fb.data() + yy * stride + x * 4;
        const char* b = g_prev.data() + yy * stride + x * 4;
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
        rfbMarkRectAsModified(g_screen, run_x0, y, run_x1, yh);
        run_x0 = -1;
        run_x1 = -1;
      }
    }
    if (run_x0 >= 0) {
      rfbMarkRectAsModified(g_screen, run_x0, y, run_x1, yh);
    }
  }
}

bool listen_ok(rfbScreenInfoPtr screen) {
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
    const int wsa = WSAGetLastError();
    std::fprintf(stderr,
                 "spike_host: probe select failed WSA=%d (listenSock=%llu maxFd=%d)\n", wsa,
                 static_cast<unsigned long long>(screen->listenSock), screen->maxFd);
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  SetProcessDPIAware();

  if (!capture_desktop()) {
    std::fprintf(stderr, "spike_host: capture failed\n");
    return 1;
  }

  g_screen = rfbGetScreen(&argc, argv, g_width, g_height, 8, 3, 4);
  if (!g_screen) {
    std::fprintf(stderr, "spike_host: rfbGetScreen failed\n");
    return 1;
  }

  g_screen->frameBuffer = g_fb.data();
  g_screen->desktopName = const_cast<char*>("road-desk media spike");
  g_screen->alwaysShared = TRUE;
  g_screen->ptrAddEvent = on_ptr;
  g_screen->kbdAddEvent = on_kbd;
  g_screen->newClientHook = on_new_client;
  g_screen->authPasswdData = reinterpret_cast<void*>(kPasswordList);
  g_screen->passwordCheck = rfbCheckPasswordByList;
  apply_server_format(g_screen);
  g_screen->httpDir = nullptr;
  g_screen->httpPort = 0;
  // Lower deferral → less drag ghosting (trade bandwidth).
  g_screen->deferUpdateTime = 1;
  if (g_screen->port > 0) {
    g_screen->autoPort = TRUE;
  }

  rfbInitServer(g_screen);

  if (!listen_ok(g_screen)) {
    std::fprintf(stderr,
                 "spike_host: VNC listen failed.\n"
                 "  - Port busy? Stop other VNC on 5900–5999, or pass: -rfbport 5910\n"
                 "  - Firewall / privilege issues on this Host?\n"
                 "  Refusing to run (avoids rfbCheckFds: select spam).\n");
    release_capture();
    rfbScreenCleanup(g_screen);
    return 1;
  }

  std::printf("spike_host listening on port %d (password: %s). Ctrl+C to stop.\n",
              g_screen->port, kPassword);
  std::fflush(stdout);

  while (rfbIsActive(g_screen)) {
    if (g_screen->listenSock == RFB_INVALID_SOCKET) {
      std::fprintf(stderr, "spike_host: listen socket lost, exiting\n");
      break;
    }

    if (g_clients > 0) {
      if (capture_desktop()) {
        // Keep frameBuffer pointer stable — never swap the vector LibVNC reads.
        g_screen->frameBuffer = g_fb.data();
        mark_dirty_blocks();
        rfbProcessEvents(g_screen, 5000);
        // Update reference frame after send (full copy; safe vs pointer swap).
        if (g_prev.size() == g_fb.size()) {
          std::memcpy(g_prev.data(), g_fb.data(), g_fb.size());
        } else {
          g_prev = g_fb;
        }
      } else {
        rfbProcessEvents(g_screen, 5000);
      }
    } else {
      rfbProcessEvents(g_screen, 50000);
    }
  }

  rfbShutdownServer(g_screen, TRUE);
  rfbScreenCleanup(g_screen);
  release_capture();
  return 0;
}
