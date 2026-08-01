#pragma once

// Private to media adapter (LibVNC host). Not part of the public media API.

#include <rfb/rfb.h>

namespace road_desk::media {

void inject_pointer(int button_mask, int x, int y);
void inject_key(rfbBool down, rfbKeySym keysym);
void release_injected_modifiers();

}  // namespace road_desk::media
