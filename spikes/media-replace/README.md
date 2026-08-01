# Track P — media-replace spike

Private mux over Schannel TLS. **No LibVNC.** Does not touch `src/media/libvnc_*`.

## Build

```powershell
.\scripts\build.ps1
```

Artifacts: `../../build/media-replace/replace_host.exe`, `replace_viewer.exe`

## G1 smoke (TLS + Control auth)

```powershell
# Host
..\..\build\media-replace\replace_host.exe 5902 road-desk
# note fingerprint from stderr

# Viewer (other shell)
$env:ROAD_DESK_TLS_INSECURE='0'
..\..\build\media-replace\replace_viewer.exe 127.0.0.1:5902 road-desk <fingerprint>
```

Expect: `G1 PASS auth ok desktop=WxH`. Wrong password → exit 2.

## G2 / G3 (Video + Input)

Same connect args. After auth, Viewer opens a window: shows GDI desktop and sends mouse/keyboard on Input channel.

Host log: `G3 Video+Input`, `sent frames=...`  
Viewer log: `opening G3 window`, `video frame id=...`

Click inside the Viewer window to control the remote. Close window to end session.

## Layout

| Path | Role |
|------|------|
| `src/protocol.h` | channel / control / video types |
| `src/mux.*` | length-framed mux + Control helpers |
| `src/capture.*` | GDI primary capture |
| `src/replace_host.cpp` | listen, TLS, PSK, Video send |
| `src/replace_viewer.cpp` | connect, TLS, auth, blit window |

Next: G3 Input channel + priority.
