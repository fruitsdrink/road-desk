# Linux/macOS GCC/Clang memory-check tools

## ASan + UBSan (GCC and Clang)

```bash
cmake -S . -B build-asan \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=1 ./build-asan/<target> ...
```

Covers OOB, use-after-free, double-free, invalid free, leaks (LSan via
`detect_leaks=1`), and UB (overflow, bad shift, null deref). Stops at the
first error by default.

## TSan (races)

`-fsanitize=thread` — cannot be combined with ASan.

## MSan (uninitialized reads, Clang only)

`-fsanitize=memory` — requires every dependency to be instrumented.

## Valgrind memcheck

```bash
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./<target>
```

Leak kinds: definite > indirect > possible; reachable = still referenced (by
design or not). `--tool=helgrind` for races.

## Static analyzers

```bash
cppcheck --enable=warning,performance,portability --inconclusive --error-exitcode=1 src/
clang-tidy -p build -checks='bugprone-*,clang-analyzer-*,performance-*,modernize-*' src/
```

`clang-analyzer-*` covers uninit, leak, and use-after-free path checks;
`bugprone-*` covers signed/unsigned, integer overflow, and wrong `new[]`/`delete`.