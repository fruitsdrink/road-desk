#pragma once

namespace road_desk::replace {

// Host-side injection (no LibVNC). Coordinates are desktop pixels.
void inject_pointer(int button_mask, int x, int y);
void inject_vk(unsigned vk, bool down, bool extended = false);
void release_modifiers();

}  // namespace road_desk::replace
