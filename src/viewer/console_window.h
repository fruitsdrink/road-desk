#pragma once

#include "connect_config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace road_desk::viewer {

// Radmin-style console: menu/toolbar/status placeholders, demo tree, list, session tabs.
int run_console(HINSTANCE instance, int show_cmd, const ConnectDefaults& connect);

// Legacy single session window (CLI host:port).
int run_direct_session(HINSTANCE instance, int show_cmd, const ConnectDefaults& connect);

}  // namespace road_desk::viewer
