# C++ Memory Bug Checklist

Walk this checklist during review. For each row: grep the pattern, read the
surrounding lifetime/ownership, and record a finding only with evidence
(file:line + reason). "How to check" gives `rg` patterns; adjust to the repo.

## 1. Lifetime & dangling pointers

| Check | Why it bites | How to check |
|---|---|---|
| Use-after-free: object deleted while other pointer/reference still used | Classic UAF, hard to repro | `rg "delete "`; trace all holders of the object; look for cached raw pointers in members/callbacks |
| Function returns pointer/reference to stack or temporary | Reference to destroyed local | `rg "return (std::move\()?&?\w+"` — returned object must be heap/out-param or moved by value |
| Container iterator/pointer invalidation | `std::vector` realloc, `std::map` erase, `std::string` modify invalidates refs | `rg "\.push_back|\.emplace|\.erase|\.reserve"` — check cached iterators/`data()` pointers survive |
| Callback/observer holds raw pointer to destroyed owner | window proc, thread callback, timer, event handler | `rg "SetTimer|CreateThread|std::thread|SetWindowLongPtr|RegisterClass"` — owner must outlive callback |
| `this` captured in async lambda | object freed before callback runs | `rg "\[.*this.*\]"` in async code; verify lifetime or use shared ownership |
| Worker thread touches object after owner thread freed it | cross-thread UAF | review teardown order: stop callbacks → join threads → destroy objects |

## 2. Double-free / invalid free

| Check | How to check |
|---|---|
| `delete` on the same pointer twice (incl. two owners) | `rg "delete "` — every pointer has exactly one owner |
| `delete` on stack/global object | every `delete` must have a matching `new` |
| `new[]` freed with `delete` (and vice versa) | `rg "new \[|delete \[?"` — match array-ness |
| Cross-allocator free (`malloc`+`delete`, `new`+`free`, `LocalFree` vs `delete`) | `rg "malloc|LocalAlloc|CoTaskMemAlloc|HeapAlloc"` near frees |
| `free()` on a C++ object with destructor | `rg "free\("` — never on `new` objects |

## 3. Buffer overflows (heap/stack/global)

| Check | How to check |
|---|---|
| Off-by-one / missing null terminator | every `MultiByteToWideChar`/`WideCharToMultiByte`/`GetWindowTextLength`/`DragQueryFileW` sizing: allocate `len+1`, null-terminate |
| Fixed buffer + `strcpy`/`strcat`/`sprintf` | `rg "strcpy|strcat|sprintf|wcscpy|lstrcpy"` → bounded copy or `std::string` |
| `memcpy`/`memmove` size mismatch | `rg "memcpy|memmove"` — verify sizes (chars vs bytes!) |
| Index/pointer arithmetic past array end | `rg "\w+\s*\[\w+\]"` on fixed arrays; loop bounds vs `size()-1` |
| Write into vector via raw `data()` without capacity | `rg "\.data\(\)"` next to `memcpy` |
| Windows W/A buffer unit confusion | returned count is in WCHARs/chars, not bytes — the #1 undersized-buffer cause |

## 4. Uninitialized memory

| Check | How to check |
|---|---|
| POD local/struct read before init | structs passed to APIs (`STARTUPINFO`, `WSADATA`, `DEVMODE`, `BITMAPINFO`) — zero them before use |
| `new T` vs `new T()` for POD (uninitialized) | `rg "new (\w+)(?!\(\))"` — PODs need `()` |
| Members without constructor init | `/analyze` C6001 / `-Wuninitialized`; check class member lists |
| `memset`/`ZeroMemory` on non-trivial type | `rg "memset|ZeroMemory"` — only safe on POD/trivial structs |
| Reading fields Windows API didn't fill | MSDN: many APIs require input fields set first (`GetMonitorInfo` cbSize, etc.) |

## 5. Memory leaks

| Check | How to check |
|---|---|
| Raw `new`/`malloc` without owner or RAII | `rg "new |malloc\("` — every owning allocation maps to `unique_ptr`/container or paired delete |
| Leak on early-return / exception path | review error paths between acquire and release |
| Overwriting a live pointer without freeing old value | `rg "= new | = malloc"` assignments to live pointers |
| Container of raw owning pointers never freed at teardown | `rg "vector<\s*\w+\s*\*"` — clear loop or `unique_ptr` |
| Thread-local / static storage allocations | allocations in static init, `thread_local`, singletons — bounded? |
| Per-frame / per-session allocations not released (media pipeline) | review capture→encode→transfer buffers and record lifecycle; release on session end |
| **Windows handles**: `CreateFile`, `FindFirstFile`, `RegOpenKey`, `OpenProcess`, `CreateEvent`, `CreateThread`, `socket` | pair each with `CloseHandle`/`closesocket` on every path |
| **GDI objects**: `CreateCompatibleDC/Bitmap/Font/Brush`, `GetDC` vs `ReleaseDC`, `SelectObject` restore | `DeleteObject`/`ReleaseDC` incl. error paths; per-frame GDI leaks = "memory grows during session" |
| **COM refcounts**: `CoCreateInstance`/`QueryInterface`/`AddRef` must be `Release`d | `rg "CoCreateInstance|QueryInterface|AddRef"` — check all exits |
| `CoTaskMemAlloc` freed with something other than `CoTaskMemFree` | `rg "CoTaskMemAlloc"` |
| `MapViewOfFile` without `UnmapViewOfFile` | `rg "MapViewOfFile"` |

## 6. Integer / sizing bugs

| Check | How to check |
|---|---|
| `count * size` overflow before allocation | `rg "\*\s*(sizeof|\w+)"` in allocation sizing; use checked math |
| Negative/underflow sizes from signed arithmetic | casts of `int` lengths to `size_t`; `GetWindowTextLength` returns `int` |
| `size_t`→`int`/`DWORD` truncation | `rg "\(int\)|\(DWORD\)|\(UINT\)"` near sizes |
| Chars vs bytes in buffer sizes (W APIs) | see §3 — the most common undersized-buffer cause |

## 7. RAII / ownership discipline

| Check | How to check |
|---|---|
| Class with raw owning members + default copy → double ownership | `rg "\* \w+;"` members; enforce Rule of Three/Five or `unique_ptr` |
| Constructor allocates then throws → leak | review constructors with raw allocations |
| Non-virtual destructor where deleted via base | polymorphic `delete`; check `virtual ~` |
| Exception thrown between acquire and release | wrap in scope guard / RAII |
| Owning vs non-owning pointers not distinguishable | name borrowed pointers (`_view`/`_borrowed`) or `gsl::not_null` |

## 8. Concurrency

| Check | How to check |
|---|---|
| Two threads delete/free the same resource | shared raw pointers; single ownership or shared ownership with clear rules |
| Async callback uses freed object | see §1; teardown order matters |
| Torn read/write of shared pointer/primitive | data race: mutex/atomic; TSan catches at runtime |

## 9. Cross-build / portability

- Same CRT across DLL boundaries on Windows: allocate in one CRT, free in another → heap corruption. Keep allocation/free inside the same module.
- `_DEBUG`/`NDEBUG` and CRT type (`/MD` vs `/MDd`) mismatch between exe and dll.
- x64: pointer-to-int truncation (`DWORD` for sizes), `%u` for pointers.

## Dynamic test checklist (after the static pass)

- [ ] Sanitizer build runs the main scenario (session/drag/record) and exits clean
- [ ] Long soak (hours / thousands of frames) with stable memory and handle counts
- [ ] Shutdown path produces no leak report
- [ ] Rapid start/stop cycles don't grow RSS
- [ ] Crash repro under AppVerifier / page heap if corruption is suspected

## Triage rules

- Static hits are suspects, not bugs — reproduce before fixing.
- One root cause per finding; group duplicates.
- Critical/High block release; Medium/Low can be scheduled.