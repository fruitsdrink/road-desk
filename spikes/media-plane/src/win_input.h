#pragma once

#include <rfb/rfb.h>

// Map RFB/X11 keysyms and pointer events to Win32 SendInput (shared-input style).

void spike_inject_pointer(int button_mask, int x, int y);
void spike_inject_key(rfbBool down, rfbKeySym keysym);
// Force KEYUP for Shift/Ctrl/Alt/Win — clears sticky modifiers after focus loss.
void spike_release_modifiers();
