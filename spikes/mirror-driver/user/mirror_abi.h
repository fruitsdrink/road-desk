#pragma once

// Shared ABI: GM1 control device IOCTL + GM2 display ExtEscape.

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define ROAD_DESK_MIRROR_DEVICE_NAME_USER "\\\\.\\RoadDeskMirror"
#define ROAD_DESK_MIRROR_ABI_VERSION 1u
#define ROAD_DESK_MIRROR_DISPLAY_NAME "Road Desk Mirror Driver"

#define ROAD_DESK_MIRROR_DEVICE_TYPE 0x8000u

#ifndef CTL_CODE
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

// Must match driver/display/dirty.h
#define RDM_ESC_GET_DIRTY 0x52444D01u
#define RDM_ESC_GET_INFO 0x52444D02u

enum RdmStatusFlags {
  kRdmStatusLoaded = 1u << 0,
  kRdmStatusCaptureReady = 1u << 1,
  kRdmStatusXpdmAttached = 1u << 2
};

#pragma pack(push, 1)
typedef struct RdmStatus {
  uint32_t abi_version;
  uint32_t flags;
  uint32_t driver_build;
  uint32_t reserved;
} RdmStatus;

typedef struct RdmInfo {
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint32_t format;
} RdmInfo;

typedef struct RdmRect {
  int32_t x;
  int32_t y;
  int32_t w;
  int32_t h;
} RdmRect;

typedef struct RdmDirtyHeader {
  uint32_t seq;
  uint32_t count;
  uint32_t max_rects;
  uint32_t reserved;
} RdmDirtyHeader;
#pragma pack(pop)

#define RDM_DIRTY_MAX_RECTS 256
#define RDM_DIRTY_BUF_BYTES \
  (sizeof(RdmDirtyHeader) + RDM_DIRTY_MAX_RECTS * sizeof(RdmRect))

#ifdef __cplusplus
}
#endif
