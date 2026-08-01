#include "dirty.h"

static HSEMAPHORE g_Lock = NULL;
static RECTL g_Rects[RDM_DIRTY_MAX];
static ULONG g_Count = 0;
static ULONG g_Seq = 0;
static ULONG g_Cx = 0;
static ULONG g_Cy = 0;
static ULONG g_Pitch = 0;
static ULONG g_BitCount = 32;
static BOOL g_Full = FALSE;

VOID RdmDirtyInit(ULONG cx, ULONG cy, ULONG pitch, ULONG bitCount) {
  if (g_Lock == NULL) {
    g_Lock = EngCreateSemaphore();
  }
  if (g_Lock) {
    EngAcquireSemaphore(g_Lock);
  }
  g_Cx = cx;
  g_Cy = cy;
  g_Pitch = pitch;
  g_BitCount = bitCount ? bitCount : 32;
  g_Count = 0;
  g_Full = FALSE;
  if (g_Lock) {
    EngReleaseSemaphore(g_Lock);
  }
}

static VOID RdmDirtyAddUnlocked(RECTL* prcl) {
  RECTL r;
  LONG w, h;

  if (!prcl || g_Cx == 0 || g_Cy == 0) {
    return;
  }
  r = *prcl;
  if (r.left > r.right) {
    LONG t = r.left;
    r.left = r.right;
    r.right = t;
  }
  if (r.top > r.bottom) {
    LONG t = r.top;
    r.top = r.bottom;
    r.bottom = t;
  }
  if (r.left < 0) {
    r.left = 0;
  }
  if (r.top < 0) {
    r.top = 0;
  }
  if (r.right > (LONG)g_Cx) {
    r.right = (LONG)g_Cx;
  }
  if (r.bottom > (LONG)g_Cy) {
    r.bottom = (LONG)g_Cy;
  }
  w = r.right - r.left;
  h = r.bottom - r.top;
  if (w <= 0 || h <= 0) {
    return;
  }

  ++g_Seq;
  if (g_Full) {
    return;
  }
  if (g_Count >= RDM_DIRTY_MAX) {
    g_Full = TRUE;
    g_Count = 1;
    g_Rects[0].left = 0;
    g_Rects[0].top = 0;
    g_Rects[0].right = (LONG)g_Cx;
    g_Rects[0].bottom = (LONG)g_Cy;
    return;
  }
  g_Rects[g_Count++] = r;
}

VOID RdmDirtyAddRect(RECTL* prcl) {
  if (!g_Lock) {
    return;
  }
  EngAcquireSemaphore(g_Lock);
  RdmDirtyAddUnlocked(prcl);
  EngReleaseSemaphore(g_Lock);
}

ULONG RdmDirtyFetch(PVOID pvOut, ULONG cjOut) {
  RDM_DIRTY_HDR* hdr;
  RDM_RECT* out;
  ULONG i, need, n;

  if (!pvOut || cjOut < sizeof(RDM_DIRTY_HDR) || !g_Lock) {
    return 0;
  }

  EngAcquireSemaphore(g_Lock);
  n = g_Count;
  need = sizeof(RDM_DIRTY_HDR) + n * sizeof(RDM_RECT);
  if (cjOut < need) {
    EngReleaseSemaphore(g_Lock);
    return 0;
  }
  hdr = (RDM_DIRTY_HDR*)pvOut;
  hdr->Seq = g_Seq;
  hdr->Count = n;
  hdr->MaxRects = RDM_DIRTY_MAX;
  hdr->Reserved = 0;
  out = (RDM_RECT*)(hdr + 1);
  for (i = 0; i < n; ++i) {
    out[i].x = g_Rects[i].left;
    out[i].y = g_Rects[i].top;
    out[i].w = g_Rects[i].right - g_Rects[i].left;
    out[i].h = g_Rects[i].bottom - g_Rects[i].top;
  }
  g_Count = 0;
  g_Full = FALSE;
  EngReleaseSemaphore(g_Lock);
  return need;
}

ULONG RdmDirtyInfo(PVOID pvOut, ULONG cjOut) {
  RDM_INFO_ESC* inf;
  if (!pvOut || cjOut < sizeof(RDM_INFO_ESC)) {
    return 0;
  }
  inf = (RDM_INFO_ESC*)pvOut;
  inf->Width = g_Cx;
  inf->Height = g_Cy;
  inf->Pitch = g_Pitch ? g_Pitch : (g_Cx * ((g_BitCount + 7) / 8));
  inf->Format = 0;
  return sizeof(RDM_INFO_ESC);
}
