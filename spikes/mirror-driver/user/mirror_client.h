#pragma once

#include "mirror_abi.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Find "Road Desk Mirror Driver", attach/detach via ChangeDisplaySettingsEx.
// Returns true on success; device name like \\.\DISPLAYn written to out_name.
bool rdm_find_mirror_device(char* out_name, size_t out_len);
bool rdm_attach_mirror(char* device_name, size_t device_name_len);
bool rdm_detach_mirror(const char* device_name);
// Clear sticky Attach.ToDesktop=1 + CDS detach (safe if already detached).
bool rdm_force_detach();
// Zero Attach.ToDesktop for peer mirrors (VNC/Radmin…). Skips rdmmini / Road Desk.
void rdm_scrub_foreign_attach_registry();
// Also clears Road Desk service keys (use when detached / session end).
void rdm_scrub_all_attach_registry();
void rdm_dump_display_devices();

// CreateDC on the mirror device; caller DeleteDC.
HDC rdm_create_mirror_dc(const char* device_name);

// ExtEscape helpers (need attached mirror + valid HDC).
bool rdm_escape_get_info(HDC hdc, RdmInfo* out);
// out_buf must be at least RDM_DIRTY_BUF_BYTES; returns bytes written (0 fail).
uint32_t rdm_escape_get_dirty(HDC hdc, void* out_buf, uint32_t out_bytes);
