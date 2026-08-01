#pragma once

// Shared user/kernel ABI for Road Desk Mirror control device.
// Device: \\.\RoadDeskMirror  (kernel \Device\RoadDeskMirror)
// GM1: GET_STATUS / GET_INFO. GM2: dirty rects + framebuffer map.

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define ROAD_DESK_MIRROR_DEVICE_NAME_USER "\\\\.\\RoadDeskMirror"
#define ROAD_DESK_MIRROR_ABI_VERSION 1u

// FILE_DEVICE_UNKNOWN = 0x22
#define ROAD_DESK_MIRROR_DEVICE_TYPE 0x8000u

#ifndef CTL_CODE
// User-mode builds without winioctl.h still need the macro layout.
#define CTL_CODE(DeviceType, Function, Method, Access) \
  (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#define METHOD_BUFFERED 0
#define FILE_ANY_ACCESS 0
#endif

#define IOCTL_RDM_GET_STATUS \
  CTL_CODE(ROAD_DESK_MIRROR_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RDM_GET_INFO \
  CTL_CODE(ROAD_DESK_MIRROR_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RDM_GET_DIRTY \
  CTL_CODE(ROAD_DESK_MIRROR_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RDM_MAP_FB \
  CTL_CODE(ROAD_DESK_MIRROR_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

enum RdmStatusFlags {
  kRdmStatusLoaded = 1u << 0,
  kRdmStatusCaptureReady = 1u << 1,  // GM2: dirty+fb available
  kRdmStatusXpdmAttached = 1u << 2   // GM2: display mirror attached
};

#pragma pack(push, 1)
typedef struct RdmStatus {
  uint32_t abi_version;
  uint32_t flags;
  uint32_t driver_build;  // yyyymmdd style or monotonic
  uint32_t reserved;
} RdmStatus;

typedef struct RdmInfo {
  uint32_t width;
  uint32_t height;
  uint32_t pitch;   // bytes per row
  uint32_t format;  // 0 = BGRA32
} RdmInfo;

typedef struct RdmRect {
  int32_t x;
  int32_t y;
  int32_t w;
  int32_t h;
} RdmRect;

typedef struct RdmDirtyHeader {
  uint32_t seq;
  uint32_t count;     // number of RdmRect following
  uint32_t max_rects; // capacity echoed by driver
  uint32_t reserved;
} RdmDirtyHeader;
#pragma pack(pop)

#ifdef __cplusplus
}
#endif
