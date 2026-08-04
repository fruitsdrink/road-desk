# Windows / MSVC memory-check tools

## MSVC AddressSanitizer (VS 2022 17.7+, Win10+)

Covers heap/stack/global OOB, use-after-free, double-free, invalid free. Leak
detection on Windows is limited — combine with the leak tools below.

```powershell
cmake -S . -B build-asan -G Ninja ^
  -DCMAKE_CXX_FLAGS="/fsanitize=address /Zi /Od /Ob0" ^
  -DCMAKE_EXE_LINKER_FLAGS="/fsanitize=address"
```

Run the scenario; ASan prints the report and stack to stderr. Fix the first
error, rebuild, re-run until clean. `ASAN_OPTIONS=detect_leaks=1` exists on
recent VS but don't rely on it.

Note: MSVC ASan needs a recent Windows SDK; the Win7 line can't run it — use
CRT debug heap + Dr. Memory / AppVerifier there.

## CRT debug heap (all MSVC, incl. Win7)

Built into the debug CRT — the zero-dependency leak check:

```cpp
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
// at startup:
_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
// break on a specific allocation (from the leak report):
// _CrtSetBreakAlloc(1234);
```

Build with debug CRT (`/MDd`). At exit the report appears in the output window
as `Detected memory leaks!` with allocation blocks. Narrow the window with
`_CrtMemCheckpoint` / `_CrtMemDumpAllObjectsSince`.

## Application Verifier (Microsoft, official)

Catches heap corruption, invalid handles, and leaks early — best for crashes:

```powershell
appverif.exe -enable heaps handles leaks -for "<exe>"
# run the scenario
appverif.exe -disable * -for "<exe>"
```

Full page heap makes the failing write fault immediately, with a stack.

## UMDH + GFlags (Microsoft, official) — heap-growth attribution

```powershell
gflags /p /enable "<exe>" /full     # stack traces on allocation (+ust)
umdh -pn:<exe> -f:snap1.txt          # snapshot 1
# run the workload, let memory grow
umdh -pn:<exe> -f:snap2.txt          # snapshot 2
umdh snap1.txt snap2.txt -d -g       # diff by growth; biggest delta = leak site
gflags /p /disable "<exe>"
```

## Dr. Memory (open source, Windows)

`drmemory.exe -- <exe> <args>` — catches uninitialized reads, OOB, invalid
frees, and leaks (`-light` for leak-only, faster). Works on Win7+.

## WinDbg / cdb on a crash dump

- `!analyze -v` — automated analysis, often pins heap corruption to a function
- `!heap -p -a <address>` — which heap block, who freed/allocated it
- `!gflag +hpa` — full page heap for the process

## Windows API gotchas (highest incidence)

- `GetWindowTextLength` returns `int`, **excluding** the null → allocate `len+1` WCHARs.
- `DragQueryFileW` returns filename length in **WCHARs, excluding null** → `need+1` WCHARs (this exact off-by-one was fixed in `mux_file_xfer.cpp`).
- `MultiByteToWideChar`/`WideCharToMultiByte` return a count in **characters** while `cbMultiByte` is **bytes** — easy to mix.
- Per-frame GDI objects (bitmaps, DCs) leak silently during capture → memory grows over a session; compare `GetGuiResources` before/after a soak.
- CRT mismatch across a DLL boundary (`new` in one module, `delete` in another) → heap corruption; keep ownership local.