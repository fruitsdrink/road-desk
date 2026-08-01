#include "inject.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace road_desk::replace {
namespace {

int g_last_buttons = 0;
// Keys we injected as down (Host-side). Cleared on KEYUP / release_all.
bool g_injected_down[256]{};

void send_mouse(DWORD flags, LONG x, LONG y, DWORD data = 0) {
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dx = x;
  in.mi.dy = y;
  in.mi.mouseData = data;
  in.mi.dwFlags = flags;
  SendInput(1, &in, sizeof(INPUT));
}

bool is_extended_vk(WORD vk) {
  switch (vk) {
    case VK_RMENU:
    case VK_RCONTROL:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_UP:
    case VK_RIGHT:
    case VK_DOWN:
    case VK_NUMLOCK:
    case VK_DIVIDE:
    case VK_SNAPSHOT:
      return true;
    default:
      return false;
  }
}

void send_key(WORD vk, bool down, bool extended) {
  INPUT in{};
  in.type = INPUT_KEYBOARD;
  in.ki.wVk = vk;
  in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
  DWORD flags = down ? 0 : KEYEVENTF_KEYUP;
  if (extended || is_extended_vk(vk)) {
    flags |= KEYEVENTF_EXTENDEDKEY;
  }
  in.ki.dwFlags = flags;
  SendInput(1, &in, sizeof(INPUT));
}

// Dual KEYUP (vk + scancode-only) — more reliable for clearing sticky modifiers.
void force_key_up(WORD vk, bool extended) {
  const WORD scan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
  INPUT ins[2]{};
  for (int i = 0; i < 2; ++i) {
    ins[i].type = INPUT_KEYBOARD;
    ins[i].ki.wScan = scan;
    ins[i].ki.dwFlags = KEYEVENTF_KEYUP;
    if (extended || is_extended_vk(vk)) {
      ins[i].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
  }
  ins[0].ki.wVk = vk;
  ins[1].ki.wVk = 0;
  ins[1].ki.dwFlags |= KEYEVENTF_SCANCODE;
  SendInput(2, ins, sizeof(INPUT));
  // Legacy fallback — some Win7 paths only clear via keybd_event.
  keybd_event(static_cast<BYTE>(vk), static_cast<BYTE>(scan),
              KEYEVENTF_KEYUP | ((extended || is_extended_vk(vk)) ? KEYEVENTF_EXTENDEDKEY : 0), 0);
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

void inject_vk(unsigned vk, bool down, bool extended) {
  if (vk == 0 || vk > 0xFE) {
    return;
  }
  send_key(static_cast<WORD>(vk), down, extended);
  if (down) {
    g_injected_down[vk] = true;
  } else {
    g_injected_down[vk] = false;
  }
}

void release_modifiers() {
  // Everything we still think is down (Ctrl/Alt/letters/…).
  for (unsigned vk = 1; vk < 256; ++vk) {
    if (g_injected_down[vk]) {
      force_key_up(static_cast<WORD>(vk), is_extended_vk(static_cast<WORD>(vk)));
      g_injected_down[vk] = false;
    }
  }
  // Always clear modifiers — covers KEYUPs lost on Viewer close / Host Ctrl+C.
  static const WORD kMods[] = {VK_LSHIFT,   VK_RSHIFT, VK_SHIFT,    VK_LCONTROL, VK_RCONTROL,
                               VK_CONTROL,  VK_LMENU,  VK_RMENU,    VK_MENU,     VK_LWIN,
                               VK_RWIN};
  for (WORD vk : kMods) {
    force_key_up(vk, is_extended_vk(vk) || vk == VK_RSHIFT || vk == VK_RCONTROL || vk == VK_RMENU ||
                         vk == VK_RWIN);
  }
  // Only UP buttons we injected. Blind RIGHTUP opens the Win7 console Edit menu
  // (seen at Host boot and on Ctrl+C).
  if (g_last_buttons & 1) {
    send_mouse(MOUSEEVENTF_LEFTUP, 0, 0);
  }
  if (g_last_buttons & 4) {
    send_mouse(MOUSEEVENTF_RIGHTUP, 0, 0);
  }
  if (g_last_buttons & 2) {
    send_mouse(MOUSEEVENTF_MIDDLEUP, 0, 0);
  }
  g_last_buttons = 0;
}

}  // namespace road_desk::replace
