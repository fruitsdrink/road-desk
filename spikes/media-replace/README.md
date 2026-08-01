# Track P — media-replace spike

Private mux over Schannel TLS. **No LibVNC.** Does not touch `src/media/libvnc_*`.  
M2：可选自研 Mirror 脏区采屏（与 [mirror-driver](../mirror-driver/) 双轨）。

## Build

```powershell
.\scripts\build.ps1
```

Artifacts: `../../build/media-replace/replace_host.exe`, `replace_viewer.exe`

## Run

```powershell
# Host — capture: auto (default) | mirror | gdi
..\..\build\media-replace\replace_host.exe 5902 road-desk
# or: replace_host.exe 5902 road-desk mirror
# or: $env:ROAD_DESK_CAPTURE='gdi'

# Viewer
$env:ROAD_DESK_TLS_INSECURE='0'
..\..\build\media-replace\replace_viewer.exe 127.0.0.1:5902 road-desk <fingerprint>
```

Expect Host: `capture=mirror device=\\.\DISPLAYn` when XPDM is installed; else `capture=gdi`.

M2 checklist: [docs/m2-checklist.md](./docs/m2-checklist.md)

## Layout

| Path | Role |
|------|------|
| `src/protocol.h` | channel / control / video types |
| `src/mux.*` | length-framed mux + Control helpers |
| `src/capture.*` | GDI primary capture |
| `src/mirror_capture.*` | Mirror attach + ExtEscape dirties + blit |
| `src/replace_host.cpp` | listen, TLS, PSK, Video send |
| `src/replace_viewer.cpp` | connect, TLS, auth, blit window |
