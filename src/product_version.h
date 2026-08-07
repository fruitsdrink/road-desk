#pragma once

// Keep in sync with CMake project(road-desk VERSION …) and host-agent.manifest.
#define ROAD_DESK_VERSION_MAJOR 0
#define ROAD_DESK_VERSION_MINOR 4
#define ROAD_DESK_VERSION_PATCH 43
#define ROAD_DESK_VERSION_STRING "0.4.43"

// Compile-time stamp (changes every rebuild) — use with VERSION_STRING in logs.
#define ROAD_DESK_BUILD_DATE __DATE__
#define ROAD_DESK_BUILD_TIME __TIME__
