#include "inject.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace road_desk::replace {
namespace {

int g_last_buttons = 0;

void send_mouse(DWORD flags, LONG x, LONG y, DWORD data = 0) {
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dx = x;
  in.mi.dy = y;
  in.mi.mouseData = data;
  in.mi.dwFlags = flags;
  SendInput(1, &in, sizeof(INPUT));
}

void send_key(WORD vk, bool down) {
  INPUT in{};
  in.type = INPUT_KEYBOARD;
  in.ki.wVk = vk;
  in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
  SendInput(1, &in, sizeof(INPUT));
}

}  // namespace

void inject_pointer(int button_mask, int x, int y) {
  const int screen_w = GetSystemMetrics(SM_CXSCREEN);
  const int screen_h = GetSystemMetrics(SM_CYSCREEN);
  if (screen_w <= 1 || screen_h <= 1) {
    return;
  }
  if (x < 0) {
    x = 0;
  }
  if (y < 0) {
    y = 0;
  }
  if (x >= screen_w) {
    x = screen_w - 1;
  }
  if (y >= screen_h) {
    y = screen_h - 1;
  }

  const LONG abs_x = (x * 65535) / (screen_w - 1);
  const LONG abs_y = (y * 65535) / (screen_h - 1);
  send_mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, abs_x, abs_y);

  const int down = button_mask & ~g_last_buttons;
  const int up = g_last_buttons & ~button_mask;
  if (down & 1) {
    send_mouse(MOUSEEVENTF_LEFTDOWN, 0, 0);
  }
  if (up & 1) {
    send_mouse(MOUSEEVENTF_LEFTUP, 0, 0);
  }
  if (down & 4) {
    send_mouse(MOUSEEVENTF_RIGHTDOWN, 0, 0);
  }
  if (up & 4) {
    send_mouse(MOUSEEVENTF_RIGHTUP, 0, 0);
  }
  if (down & 2) {
    send_mouse(MOUSEEVENTF_MIDDLEDOWN, 0, 0);
  }
  if (up & 2) {
    send_mouse(MOUSEEVENTF_MIDDLEUP, 0, 0);
  }
  g_last_buttons = button_mask;
}

void inject_vk(unsigned vk, bool down) {
  if (vk == 0 || vk > 0xFE) {
    return;
  }
  send_key(static_cast<WORD>(vk), down);
}

void release_modifiers() {
  send_key(VK_SHIFT, false);
  send_key(VK_CONTROL, false);
  send_key(VK_MENU, false);
  send_key(VK_LWIN, false);
  send_key(VK_RWIN, false);
  g_last_buttons = 0;
}

}  // namespace road_desk::replace
