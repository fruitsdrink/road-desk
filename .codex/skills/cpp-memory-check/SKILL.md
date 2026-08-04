---
name: cpp-memory-check
description: >-
  Audit C++/C code for memory bugs: memory leaks, use-after-free, double-free,
  invalid free, buffer overflows, dangling pointers, uninitialized reads,
  new[]/delete[] mismatch, integer-overflow buffer sizing, iterator
  invalidation, and Windows handle/GDI/COM leaks. Use when asked to find or fix
  memory leaks or pointer bugs, review C++ code for memory safety, investigate
  heap corruption, crash-on-exit, or session-record leaks, run sanitizers
  (ASan/UBSan/TSan), Valgrind, Dr. Memory, Application Verifier, UMDH,
  cppcheck, or clang-tidy, or audit a C++ project (MSVC on Windows, or
  GCC/Clang) for memory-related issues.
---

# C++ Memory Check

Run a structured memory-safety audit and verify fixes with instrumented builds.

## Workflow

1. **Scope** — Identify modules to audit and the toolchain (MSVC vs GCC/Clang; Win7/Win10 differences), plus a repeatable scenario (a session, a drag, a record round-trip).
2. **Static review first** — Walk [references/checklist.md](references/checklist.md) with `rg`. Flag all hits, then triage by severity. Never fix before triage.
3. **Instrumented build** — Build with sanitizers in a separate build dir (never ship it):
   - Windows/MSVC → [references/windows-tools.md](references/windows-tools.md)
   - Linux/macOS GCC/Clang → [references/posix-sanitizers.md](references/posix-sanitizers.md)
   Run the scenario under the instrumented build. ASan stops at the first error: fix → rebuild → re-run until clean.
4. **Leak / corruption tools** — Long-lived daemon: CRT leak dump / UMDH snapshots / handle+GDI counters. Short-lived tool: Valgrind or Dr. Memory full leak report. Crashes: AppVerifier + page heap to reproduce earliest.
5. **Report** — For each finding: `file:line`, bug class, evidence, fix. Lead with Critical/High.

## Severity

| Level | Meaning | Examples |
|-------|---------|---------|
| Critical | Memory corruption/race, can crash anywhere | use-after-free, double-free, heap OOB write, cross-thread delete |
| High | Definite leak/overflow in hot or long-lived path; OOB read; uninit read | per-frame GDI/handle leak, per-session leak, off-by-one |
| Medium | Issue in one-shot path or risky pattern unproven | exit-time leak, iterator invalidation risk |
| Low | Hardening; no current repro | missing RAII where lifetime is locally safe |

## Hard rules

- Prefer RAII / `unique_ptr` over raw owning pointers; never "fix" a leak by leaking at exit.
- Never apply sanitizer flags to release/production builds.
- Windows text-length APIs (`GetWindowTextLength`, `MultiByteToWideChar`, `DragQueryFileW`): allocate `len+1`, count in the API's unit (chars vs bytes), reserve null terminator.
- Match allocator families: `new`/`delete`, `new[]`/`delete[]`, `malloc`/`free`, `CoTaskMemAlloc`/`CoTaskMemFree`.
- GDI objects / HANDLEs must be released on every path, including early returns.