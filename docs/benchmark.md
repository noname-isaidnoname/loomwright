# Benchmark

Measured build and test performance for Loomwright on the maintainer's
machine (CachyOS, KDE Plasma, 4 jobs). Numbers are a snapshot, not a promise:
always measure before and after a change and keep the evidence with the commit.
If one of your changes shows no gain or causes a regression, consider trying a different approach; otherwise reject it and move on.

## The runtime benchmark

`loomwright_bench` (`tests/bench_runtime.cpp`) measures two synthetic workloads
that approximate what a script does between two key presses:

- `arith` — a nested integer-arithmetic loop with a string concatenation.
- `await` — 10 000 calls to a user-defined function per repetition, so every
  call goes through the awaiting evaluator and the scheduler's coroutine
  completion path. The function itself has no `wait` or backend action; this
  is not a timer- or fd-suspension benchmark.

Neither workload prints, after 0.5.7 removed the `log` that made the old
benchmark measure stdout. The front end (parse) is measured across the same
trials. Usage:

```bash
loomwright_bench [repetitions] [trials]   # default: 20 7
taskset -c 0 ./build/release-clang/loomwright_bench 20 7
```

After a warm-up pass, each workload is timed in `trials` independent batches
of `repetitions` reps; both the best and the median time per repetition are
printed, along with the allocation count per run. The best time tracks code
generation, the median tracks a typical run. Compare the same build settings
before and after a change, and prefer pinning to one core for the lowest
noise.

## Compiler comparison

The compiler comparison from 0.4.0 still holds (same workload shape; the
numbers predate the harness rewrite): both compilers at `-O3`, minimum of
eight runs pinned to one core:

| Compiler | Run time per repetition | Binary size |
| --- | --- | --- |
| GCC 16.2.1 | 33.9 ms | 1 145 536 bytes |
| Clang 23.1.4 | 23.5 ms | 1 214 048 bytes |

Clang is about 30 % faster on this workload, so it is the reference compiler
for the release artifacts. The two binaries are about the same size. GCC stays
supported and is built by the CI workflow.

Reproduce it with:

```bash
cmake --preset release-clang && cmake --build build/release-clang -j4
./build/release-clang/loomwright_bench 20
cmake --preset release-gcc && cmake --build build/release-gcc -j4
./build/release-gcc/loomwright_bench 20
```

## Runtime work (0.5.5)

Every expression used to be evaluated by a coroutine, and every statement by
another one. A coroutine frame is allocated on the heap, and the awaiting
frame is handed back to the scheduler and resumed from there, so a script
spent most of its time in the scheduler instead of in the script. The parser
now marks the expressions and the statements that may wait, and the runtime
evaluates everything else without a coroutine:

| Build | Run time per repetition | Allocations per run |
| --- | --- | --- |
| Before | 149 ms | 1 592 000 |
| After | 23.5 ms | 29 974 |

That is 6.3× faster with 53× fewer allocations. The rest of the change is
smaller: the operator is resolved by the parser instead of by comparing
strings while the script runs, the variables and the functions live in hash
maps, the key table is searched with views instead of with strings, and the
lists share their storage through a plain reference count instead of an
atomic one.

Link-time optimization is worth about 13 % of the same benchmark, because the
runtime is split over many translation units and the inliner only sees across
them when the linker gets the bitcode.

## C++20 passes (0.5.6)

The interpreter dispatch now resolves what it can at compile time. The parser
resolves every builtin call site to a `Builtin` enumerator; `Runtime::call_builtin`
switches on the enum instead of walking a chain of string comparisons. The
builtin table is a `constexpr` array sorted by name with `static_assert`s
guarding the order and id coverage; the alias, mouse-button and common-key
tables in `src/keys.cpp` are `constexpr` arrays searched with `std::lower_bound`
and `std::binary_search` over `std::string_view`, and the generated tables in
`src/keys.inc` are covered by the existing `std::size` sanity check. The lexer
no longer carries an unused `raw` slice on every token and the newline token no
longer allocates a `"\\n"` string. The checker's tree walkers use concepts
instead of `std::function`.

The benchmark is unchanged within noise on this machine (pinned to one core,
load average ~2.6): ~25 ms per run, 29 974 allocations per run, parse still
0.01 ms per script. The pass is a codegen-quality change, not a throughput
change: the hot path no longer depends on per-call string dispatch, and the
tables cannot silently drift out of sorted order.

| Approach | Result |
| --- | --- |
| Builtin enum dispatch + constexpr tables | Kept: measured at parity, cleaner codegen |
| Lexer `Token::raw` removal | Kept: dead field, one string move saved per token |
| Concepts instead of `std::function` in checker walkers | Kept: no type erasure, no possible small-buffer spill |
| Keyword interning (`TokenKind::Keyword`) | Rejected: the language's keywords are soft and context-dependent; parse is 0.01 ms of a 25 ms workload |
| Heterogeneous lookup for `unordered_map` (`std::hash<std::string_view>`) | Rejected after measurement: 10–12 % slower (`27.6`–`28.4 ms` vs `24.5`–`25.1 ms`); surface as-is has no temporary-string call sites that benefit |

## Runtime performance pass (0.5.7)

The runtime benchmark was rewritten first: the old workload printed
`log str(total)` once per repetition, so the timing included stdout; both
workloads are now print-free, and the harness reports best and median over
independent trials instead of a single cold pass. A second workload, `await`,
forces 10 000 awaiting user-function calls per repetition so the coroutine
completion path is measured too. Code unchanged, new harness, same machine
(pinned to one core, load average ~2):

| Workload | Before (0.5.6) | After (0.5.7) |
| --- | --- | --- |
| arith | best 22.8 ms, median 23.2 ms | best ~21.3–22.2 ms, median ~21.5–22.4 ms |
| await | best ~146.8–148.8 ms, median ~147.7–149.7 ms | best ~147.4–150.7 ms, median ~147.7–154.0 ms |
| arith allocations | 29 972 per run | 29 972 per run |

Allocation counts are identical; the wins are smaller constant factors. The
changes:

- `lookup_variable` returns a borrowed pointer instead of copying the whole
  `Value` into an out-parameter (one 64-byte copy plus a string allocation
  per variable read is now one dereference).
- List concatenation reserves the target capacity instead of growing by
  doubling next to a fresh `left` copy.
- Input reads reuse the decode buffer and the `RawEvent` vector across
  batches instead of allocating per batch.
- Integer-to-string conversions use `std::to_chars` (like `format_float`
  already did) and list formatting pre-reserves an estimate.
- The scheduler queues admit O(1) deduplication: a `ready_queued` /
  `deferred_queued` flag on each frame replaces the `std::find` /
  `std::erase` linear scans in `defer_resume`, `forget_task`,
  `notify_finished` and `cancel`. Frames already queued when they finish are
  skipped at pop time via `known_`, which is also what made dropping the
  eager erases safe. `fd_waiters_` is now an `unordered_map`.

The scheduler change is asymptotically better (finish and cancel paths were
O(n) scans over queues that grow with the number of parked frames) but
measures at parity on these workloads, where queues stay short; it is kept
for dispatch-heavy scripts, not for the numbers above. All 157 unit tests
pass.

## C++23 pass (0.6.0)

The build now uses `-std=c++23`, and the runtime adopts the C++23 vocabulary
where it removes ceremony: `contains()` replaces `find(...) != npos`,
`spawn_detached` returns `std::expected<void, std::string>` instead of writing
into an error out-parameter, stateless lambdas passed to algorithms get a
`static` call operator, and the builtin table's size check uses
`std::to_underlying`. Configure probes those features once, so a compiler with
an old standard library fails with one clear message rather than a wall of
template diagnostics.

Measured as an interleaved A/B/A/B comparison: a pristine HEAD worktree
(C++20) and the ported tree (C++23) were each built with the same release
preset, then run alternately on one pinned core, 20 repetitions × 7 trials per
run. Each row is the best time of two runs per tree; medians stayed within
1 ms of the best times.

| Compiler | Workload | C++20 best (2 runs) | C++23 best (2 runs) | Change |
| --- | --- | --- | --- | --- |
| GCC 16.2.1 | arith | 30.98 / 30.88 ms | 28.96 / 28.68 ms | ~7 % faster |
| GCC 16.2.1 | await | 105.98 / 105.70 ms | 111.08 / 110.89 ms | ~5 % slower |
| Clang 23.1.5 | arith | 24.78 / 23.84 ms | 23.61 / 23.62 ms | within noise |
| Clang 23.1.5 | await | 161.16 / 160.85 ms | 161.11 / 160.78 ms | parity |

Allocation counts (29 972 per `arith` run, 171 109.9 per `await` run) and both
checksums are identical across all eight runs. The source changes are API and
readability changes, not algorithm changes; GCC's small shifts come from code
generation in the new standard mode. The point of the pass is a smaller API
surface and less search noise in the source, not throughput. All 157 unit
tests and the full CTest suite pass on both compilers.

## Performance pass (0.6.1)

Compared `4fcdbde` from a fresh `git archive` to the candidate on a
CachyOS i7-7600U (2 cores / 4 threads), with Clang 23.1.5, mold 3.0.0,
CMake 4.5.1 and Ninja 1.13.2. Builds use `release-clang`, `-j4`, new
build directories and `CCACHE_DISABLE=1`; the release optimizer and
hardening flags are unchanged. Runtime runs alternate baseline / candidate
on logical CPU 3 with 10 repetitions and five trials per invocation.

| Measurement | Baseline | Final |
| --- | --- | --- |
| Clean `-j4` wall time | 68.17 / 68.47 s | 62.15 s |
| Rebuild of unchanged `value.cpp` object and dependents | 27.88 s | 1.63 s |
| CTest `-j4` | 16.26 s | 8.06 s |
| X11 GUI integration | 3.93 s | 2.08 s |
| `arith` best / median | 29.41 / 26.74 ms | 18.25 / 19.28 ms |
| `await` best / median | 191.16 / 174.62 ms | 12.14 / 12.92 ms |
| `await` with real zero-duration timers | 203.04 / 221.65 ms | 23.47 / 23.67 ms |
| Scheduler only (10,000 timer suspensions) | 8.58 / 8.67 ms | 7.63 / 7.75 ms |
| Parse only | 7.7 / 7.5 μs | 7.3 / 7.7 μs |
| CLI `.text` | 830,848 B | 740,256 B |
| Benchmark `.text` | 498,932 B | 405,476 B |
| Unit harness `.text` | 1,806,896 B | 1,122,704 B |

The clean build gain is ~9%; `arith` improved ~25%,
`await` ~92%, and the zero-duration-timer script ~89% (not a claim that
epoll or an arbitrary real wait got nine times faster). Original script
checksums stayed `2750275000` and `9700`. Allocations per run fell from
29,972.1 to 10,003.1 (`arith`) and from 171,109.8 to 150,953.5 (`await`);
the scheduler-only checksum stayed `570000` with allocations per
repetition down from 10,159.3 to 159.3.

### Changes

- Binary expressions without calls borrow existing operands. String
  concatenation reserves the result once, and assignment moves the
  evaluated value into its destination.
- A function consisting of one call-free `return` computes the result
  without the body coroutine or an exception-based `return`; the caller
  still awaits it through the scheduler. `perf record` showed exception
  handling at the top of the baseline profile (`__gxx_personality_v0`
  alone took 10.85% of samples); the candidate is dominated by evaluation
  and hash-table work.
- The scheduler keeps the current timer ticket in the promise instead of
  allocating a hash node per suspension.
- Assertion reporting moved off individual test sites, and GTK/Wayland
  compile requirements are limited to the translation unit that uses
  them. Compile commands for `runtime.cpp` and `test_runtime.cpp` went
  from 22 system include directories to one, with `-O3` for the core,
  ThinLTO and hardening retained.
- Replacing screen-geometry regexes removed a heavy template
  instantiation: `nm -C` found 137 `std::…regex…` symbols in the baseline
  CLI and none in the candidate.
- The X11 layout probe waits for the focused window to decode a key
  under each new keyboard map instead of sleeping.

### Rejected experiments

- ThinLTO cache reuse across targets: backend objects with matching
  settings are reused (a warm-cache link took 0.07 s vs 7.31 s cold and
  matched byte-for-byte), but clean builds still optimize objects whose
  import settings differ.
- `-O0` on test-only objects (62.75 s), no ThinLTO on test translation
  units (63.51 s), lld instead of mold (67.95 s), mixed LTO objects
  (61.73 s), a multicall test binary (64.96 s), and a two-slot test
  compile pool (65.84 s): all within noise of or worse than the default
  62.15 s build.
- Symmetric transfer from `FinalAwaiter` (return the continuation
  directly): parity on `await` but ~1 ms slower on `arith`; the deferred
  path stays for the "one place frames run" invariant.

### Verification

The final Ninja log puts the test executable link at 20.21 s, the CLI
link at 18.74 s, and the benchmark link at 12.53 s; these overlap and
cannot be added to estimate wall time. A fresh paired clean build took
72.29 s at `4fcdbde` and 64.64 s on the candidate (~11% lower); all three
candidate builds produced benchmark binaries matching round two
byte-for-byte. Install-only builds (`-DBUILD_TESTING=OFF`) took 34.20 s
originally and 31.19 s on the candidate, and a warm rebuild of an
unchanged `value.cpp` and its dependents took 1.56 s full / 1.15 s
install-only (original: 27.88 s / 8.17 s) — cache-hit relinks, not
timings for an arbitrary edit.

Both picker depths, including disconnect and screenshot-pixel cases, and
the GUI integration passed five times each on one physical core, with
the graphical resource lock in place. The final picker driver verified
startup ownership and reported an injected SIGKILL failure without
hanging cleanup. Clang 23.1.5 and GCC 16.2.1 both passed all 164 unit
tests, the available integration tests, and all seven example checks;
GCC runtime medians improved from 36.47 / 37.33 ms (`arith`) and 145.98 / 148.62 ms
(`await`) to 21.36 / 21.33 ms and 15.60 / 16.71 ms with matching
checksums.

## Results at 0.4.0

| Measurement | Before | After |
| --- | --- | --- |
| Cold build (`-j4`, empty ccache) | ~69 s | ~43 s |
| Slowest compile unit (`runtime.cpp`) | 24.3 s | 11.8 s |
| `ctest` total | 39.6 s (serial) | 9.7 s (`-j4`) |
| `picker_integration` | 11.6 s | 5.1 s |
| `gui_integration` | 5.8 s | 4.0 s |

Baseline unit suite: 154 tests in 2.1 s. Current suite: 155 tests in ~2.1 s.
`loomwright_tests` itself was never the bottleneck; the integration scripts and
the cold build were.

## What produced the improvement

### Pass 2 — codegen

`src/runtime.cpp` (~2550 lines) was split into `runtime.cpp`,
`runtime_input.cpp`, `runtime_output.cpp` and `runtime_gui.cpp`, with shared
text helpers in `runtime_internal.hpp`. `tests/test_runtime.cpp` (~1784 lines)
was split into `test_runtime.cpp` and `test_runtime_language.cpp`, with shared
fixtures in `runtime_fixtures.hpp`. Smaller translation units compile faster
and invalidate less when a header changes.

### Pass 3 — test speed

The picker test used a fixed `sleep 1` settle, which approximated readiness.
The real signal is that the picker takes input focus once the overlay is
viewable and its masks are selected, so the poll interval dropped from 100 ms
to 20 ms. The GUI mapping probe dropped from a fixed 3 s to 1.5 s. The two
picker variants got disjoint display ranges (91–95 and 81–85) so CTest can run
four jobs without them fighting over an X server. They still share the
`picker_display` resource lock in this measurement because they use the same
screenshot tool, and spectacle is single-instance. The current suite uses the
broader `graphical_integration` lock; see [testing.md](testing.md).

### Pass 4 — cold build

The build is CPU-bound (195 s of compile edges ÷ 4 cores ≈ the measured wall
time). The test harness and probes now compile at `-O1`
(`loomwright_test_options`) while `loomwright_core` keeps `-O3`. The harness is not
performance critical, and the code it exercises is still optimized.

## Approaches measured and rejected

| Approach | Result |
| --- | --- |
| Unity builds | Rejected: anonymous-namespace collisions across translation units |
| Precompiled header | Rejected: only ~4 % faster, and its dependency tracking produced stale objects |
| `-O2` instead of `-O3` | No measurable difference |
| ccache single-preprocessing mode | No gain |
| Symmetric transfer from `FinalAwaiter` (return the continuation instead of deferring to the scheduler's loop) | Rejected after measurement in 0.5.7: parity on `await` (~147–150 ms) but ~1 ms slower on `arith` per run (best 22.2–22.7 ms vs 21.3–22.2 ms). The direct transfer bypasses `resume_frame`'s guards and the "one place frames run" invariant; the deferred path stays. |

## How to reproduce

```bash
# Cold builds: use fresh build directories and bypass ccache.
git archive 4fcdbde | tar -x -C "$baseline_source"  # an empty, existing directory
CCACHE_DISABLE=1 cmake --preset release-clang -S "$baseline_source" -B "$baseline_build"
time env CCACHE_DISABLE=1 cmake --build "$baseline_build" -j4 -- -d stats
CCACHE_DISABLE=1 cmake --preset release-clang -B "$candidate_build"
time env CCACHE_DISABLE=1 cmake --build "$candidate_build" -j4 -- -d stats

# Full test suite
ctest --preset release

# Individual integration tests
ctest --preset release -R picker_integration --output-on-failure
ctest --preset release -R gui_integration --output-on-failure
```

## Guidance

- Measure before and after. A claim without command output is not evidence.
- Keep `loomwright_core` at the release optimisation level; only the harness and
  probes are downgraded.
- Prefer splitting a translation unit over unity builds or PCH. Both of those
  hid breakage (collisions, stale objects) that was worse than the slow build.
- Test-speed changes should remove approximation, not add sleeps. Poll for the
  real readiness signal.
