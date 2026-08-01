#include "win_input.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <rfb/keysym.h>

namespace {

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

WORD keysym_to_vk(rfbKeySym keysym) {
  if (keysym >= XK_space && keysym <= XK_asciitilde) {
    SHORT vk = VkKeyScanA(static_cast<CHAR>(keysym & 0xff));
    if (vk != -1) {
      return static_cast<WORD>(vk & 0xff);
    }
  }
  switch (keysym) {
    case XK_BackSpace:
      return VK_BACK;
    case XK_Tab:
      return VK_TAB;
    case XK_Return:
    case XK_KP_Enter:
      return VK_RETURN;
    case XK_Escape:
      return VK_ESCAPE;
    case XK_Delete:
    case XK_KP_Delete:
      return VK_DELETE;
    case XK_Home:
    case XK_KP_Home:
      return VK_HOME;
    case XK_End:
    case XK_KP_End:
      return VK_END;
    case XK_Left:
    case XK_KP_Left:
      return VK_LEFT;
    case XK_Up:
    case XK_KP_Up:
      return VK_UP;
    case XK_Right:
    case XK_KP_Right:
      return VK_RIGHT;
    case XK_Down:
    case XK_KP_Down:
      return VK_DOWN;
    case XK_Page_Up:
    case XK_KP_Page_Up:
      return VK_PRIOR;
    case XK_Page_Down:
    case XK_KP_Page_Down:
      return VK_NEXT;
    case XK_Shift_L:
    case XK_Shift_R:
      return VK_SHIFT;
    case XK_Control_L:
    case XK_Control_R:
      return VK_CONTROL;
    case XK_Alt_L:
    case XK_Alt_R:
      return VK_MENU;
    case XK_F1:
      return VK_F1;
    case XK_F2:
      return VK_F2;
    case XK_F3:
      return VK_F3;
    case XK_F4:
      return VK_F4;
    case XK_F5:
      return VK_F5;
    case XK_F6:
      return VK_F6;
    case XK_F7:
      return VK_F7;
    case XK_F8:
      return VK_F8;
    case XK_F9:
      return VK_F9;
    case XK_F10:
      return VK_F10;
    case XK_F11:
      return VK_F11;
    case XK_F12:
      return VK_F12;
    default:
      return 0;
  }
}

int g_last_buttons = 0;

}  // namespace

void spike_inject_pointer(int button_mask, int x, int y) {
  const int screen_w = GetSystemMetrics(SM_CXSCREEN);
  const int screen_h = GetSystemMetrics(SM_CYSCREEN);
  if (screen_w <= 1 || screen_h <= 1) {
    return;
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

void spike_inject_key(rfbBool down, rfbKeySym keysym) {
  const WORD vk = keysym_to_vk(keysym);
  if (vk == 0) {
    return;
  }
  send_key(vk, down == TRUE);
}

void spike_release_modifiers() {
  // Always emit KEYUP even if we think they are up — Host may have sticky Alt/Ctrl
  // after Viewer lost focus mid-chord (Alt+double-click → Explorer Properties).
  send_key(VK_SHIFT, false);
  send_key(VK_CONTROL, false);
  send_key(VK_MENU, false);
  send_key(VK_LWIN, false);
  send_key(VK_RWIN, false);
}
