#ifndef _RDM_DIRTY_H_
#define _RDM_DIRTY_H_

#include "driver.h"

// Private ExtEscape codes (userspace: mirror_abi.h RDM_ESC_*)
#define RDM_ESC_GET_DIRTY 0x52444D01u /* 'RDM\x01' */
#define RDM_ESC_GET_INFO  0x52444D02u
#define RDM_ESC_GET_FRAME 0x52444D03u

#define RDM_DIRTY_MAX 256

#pragma pack(push, 1)
typedef struct _RDM_DIRTY_HDR {
  ULONG Seq;
  ULONG Count;
  ULONG MaxRects;
  ULONG Reserved;
} RDM_DIRTY_HDR;

typedef struct _RDM_RECT {
  LONG x;
  LONG y;
  LONG w;
  LONG h;
} RDM_RECT;

typedef struct _RDM_INFO_ESC {
  ULONG Width;
  ULONG Height;
  ULONG Pitch;
  ULONG Format; /* 0 = BGRA32-ish desktop */
} RDM_INFO_ESC;

typedef struct _RDM_FRAME_HDR {
  ULONG Width;
  ULONG Height;
  ULONG Pitch;
  ULONG Format; /* 0 = 32bpp BGRA-ish, top-down */
} RDM_FRAME_HDR;
#pragma pack(pop)

VOID RdmDirtyInit(ULONG cx, ULONG cy, ULONG pitch, ULONG bitCount);
VOID RdmDirtyAddRect(RECTL* prcl);
ULONG RdmDirtyFetch(PVOID pvOut, ULONG cjOut); /* bytes written, 0=fail */
ULONG RdmDirtyInfo(PVOID pvOut, ULONG cjOut);
/* Copy the full framebuffer into pvOut (after RDM_FRAME_HDR). src_bits is the
 * engine-locked surface pixel pointer (SURFOBJ.pvBits) — never a hand-computed
 * offset into an EngAllocMem block, which can fault/BSOD under concurrent draws. */
ULONG RdmFrameFetch(PVOID src_bits, ULONG pitch, PVOID pvOut, ULONG cjOut);

#endif /* _RDM_DIRTY_H_ */
