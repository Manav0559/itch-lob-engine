# Hardware profiling

`bench_main.cpp` times the book mutations with `std::chrono::steady_clock`,
which answers "how long did this take" but not "why" -- a slow p99 could be a
cache miss, a branch mispredict, a page fault, or just scheduler noise, and
wall-clock time alone can't tell those apart. `bench/hw_profile.sh` runs
`./build/bench` under whatever hardware/OS counter tool the current platform
actually exposes to userspace, to get closer to "why."

That tool is not the same on both CI platforms, and covering the gap
honestly is the point of this document.

## Why the Linux/macOS gap exists

Linux exposes CPU performance-monitoring-unit (PMU) counters -- cache hits
and misses, branch prediction outcomes, retired instructions, core cycles --
to unprivileged userspace through the `perf_event_open` syscall. `perf stat`
is a thin wrapper over that syscall. This works out of the box on most
GitHub Actions `ubuntu-latest` runners.

macOS does not offer an equivalent. Apple locks PMU access behind
entitlements that ordinary CLI binaries (including anything `cmake --build`
produces here) don't have, and there's no `perf_event_open`-equivalent
syscall for third-party processes to call instead. Getting real cache-miss
or branch-miss counts on macOS requires either Instruments' `xctrace`
(a full Instruments/DTrace-based toolchain, not a scriptable one-liner) or a
signed, entitled binary -- both well outside what this benchmark script
should require a contributor to install. `hw_profile.sh` does not attempt
either. It does not fake or approximate cache-miss numbers on macOS.

## What's measured on each platform

### Linux: `perf stat`

```
perf stat -e cache-misses,cache-references,branch-misses,branch-instructions,cycles,instructions -- ./build/bench
```

Six counters, all read directly from the PMU:

| counter | what it tells you |
|---|---|
| `cache-references` / `cache-misses` | last-level cache traffic and miss rate -- how often a memory access had to go past cache to DRAM |
| `branch-instructions` / `branch-misses` | how often the CPU's branch predictor guessed wrong and paid a pipeline-flush penalty |
| `cycles` / `instructions` | raw cycle and instruction counts; `instructions / cycles` gives IPC, a rough measure of how much useful work each cycle did |

### macOS: `/usr/bin/time -l`

```
/usr/bin/time -l ./build/bench
```

No cache-miss or branch-miss breakdown. What it does give, from the BSD
`getrusage`/`task_info` accounting the kernel maintains for every process
regardless of entitlements:

| field | what it tells you |
|---|---|
| `maximum resident set size` / `peak memory footprint` | peak memory the run actually touched -- catches unexpected growth (e.g. a book store that isn't clearing between passes) |
| `page reclaims` / `page faults` | how much of the run's memory access was satisfied from already-resident pages vs. required kernel involvement to map in a new page |
| `voluntary` / `involuntary context switches` | how often the process yielded the CPU vs. got preempted -- involuntary switches are noise the scheduler injected, not the benchmark's own behavior |
| `instructions retired` / `cycles elapsed` | on Apple Silicon, macOS *does* surface these two aggregate counts (unlike cache/branch counters, which stay locked down) -- enough for a coarse IPC figure, but with no cache or branch breakdown behind it |

This is a deliberately smaller, honest subset of what `perf stat` reports on
Linux. Cross-platform-comparable numbers (page faults, RSS, context
switches) exist on both; cache/branch numbers exist only on Linux.

`hw_profile.sh` detects the platform via `uname -s` and runs the matching
path automatically, printing which mode it picked before running.

## Interpreting bench_main's numbers with this data

- **A page-fault spike during the warm-up pass is expected, not a bug.**
  `run_book_type`'s warm-up pass exists specifically to absorb first-touch
  page faults and allocator growth before the three measured passes run --
  that's the comment already in `bench_main.cpp`. If `hw_profile.sh`'s
  macOS output showed a high `page faults` count, that would be consistent
  with the warm-up doing its job; a high count would be more concerning
  showing up during a measured-only run.

- **A branch-miss-heavy message type on Linux would point at unpredictable
  control flow**, and this repo has a concrete place to expect that
  difference: `OrderBook` (`include/book/order_book.hpp`) keeps its price
  ladders in `std::map`, so every `add`/`execute`/`cancel` walks a
  red-black tree whose shape depends on the current set of live price
  levels -- data-dependent branches the predictor can't warm up on across
  calls. `LadderBook` indexes a flat, tick-sized array directly from price,
  so its hot path is closer to straight-line arithmetic addressing with far
  fewer data-dependent branches. If Linux `perf stat` numbers show
  `OrderBook` with a materially higher branch-miss rate than `LadderBook`
  on the same message type, that's this structural difference showing up,
  not noise.

- **What a Linux contributor can learn that a macOS contributor structurally
  cannot get locally:** whether `OrderBook`'s p99 tail (already visibly
  wider than `LadderBook`'s in `bench/results.csv`) is a cache-miss story
  (red-black tree nodes scattered across the heap vs. LadderBook's
  contiguous array) or a branch-miss story (tree-shape-dependent
  comparisons) or both. Wall-clock time and the macOS counters above can
  show *that* the tail is wider; only Linux's `cache-misses` and
  `branch-misses` counters can show *which one* is driving it. A macOS
  contributor can still see the RSS/fault/IPC picture and correlate it
  against the same run's `bench/results.csv` latencies, but the
  cache/branch attribution step needs Linux.

## Recommended Linux CI integration (not applied in this change)

This change deliberately does not touch `.github/workflows/ci.yml`. If a
future change wants to wire `hw_profile.sh` into CI, the natural shape is a
job that only runs on the Linux leg of the existing matrix (macOS runners
would just fail the `perf` presence check):

```yaml
  hw-profile:
    name: hw-profile (ubuntu-latest)
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Configure
        run: cmake -B build -DCMAKE_BUILD_TYPE=Release
      - name: Build bench
        run: cmake --build build --target bench --parallel
      - name: Install perf
        run: sudo apt-get update && sudo apt-get install -y linux-tools-common linux-tools-generic linux-tools-`uname -r`
      - name: Hardware profile
        run: ./bench/hw_profile.sh
```

`perf_event_open` can be restricted even on Linux by the
`kernel.perf_event_paranoid` sysctl; GitHub-hosted `ubuntu-latest` runners
have historically allowed it for unprivileged `perf stat` without extra
`sysctl` changes, but a self-hosted runner may need
`sudo sysctl kernel.perf_event_paranoid=-1` (or equivalent) added to the
step above if the counters read as inaccessible.

## Example run (macOS, real -- captured on this machine)

This is real output from running `./bench/hw_profile.sh` on this Apple
Silicon macOS dev machine, unedited except for trimming the per-message-type
latency table (see `bench/README.md` for what that table means -- it's
unrelated to the hardware counters below):

```
hw_profile: macOS detected -- running under '/usr/bin/time -l'
  (macOS does not grant userspace PMU access; cache-miss/branch-miss counters are
   NOT available here without a kernel extension or special entitlements this repo
   does not require. This reports memory/fault/scheduling counters instead -- a
   deliberately smaller, honest subset. See bench/HARDWARE_PROFILING.md.)

generating synthetic session: 2200000 messages across 9 symbols, seed=1234567
replaying 75680770 bytes, 1 warm-up + 3 measured passes per book type

[OrderBook] warm-up + 3 measured passes done (books=9, unknown_refs=0)
[LadderBook] warm-up + 3 measured passes done (books=9, unknown_refs=0)

wrote bench/results.csv
        9.72 real         7.30 user         0.65 sys
           275857408  maximum resident set size
                   0  average shared memory size
                   0  average unshared data size
                   0  average unshared stack size
               96350  page reclaims
                  73  page faults
                   0  swaps
                   0  block input operations
                   0  block output operations
                   0  messages sent
                   0  messages received
                   0  signals received
                  63  voluntary context switches
               37767  involuntary context switches
         23545953952  instructions retired
         20167350162  cycles elapsed
           300089944  peak memory footprint
```

73 page faults across two full book-type runs (each doing a warm-up pass
plus 3 measured passes over a ~75MB synthetic session) is small -- consistent
with the warm-up pass doing its job of absorbing first-touch faults before
the measured passes run, per the interpretation note above. IPC here works
out to `23545953952 / 20167350162 ≈ 1.17` instructions/cycle for the whole
run (both book types combined) -- a real number from this run, not a
cache/branch-attributed one; a Linux `perf stat` run would be needed to say
whether that IPC is limited by cache misses, branch mispredicts, or
something else.

No Linux `perf stat` example is included above. This environment had no
Linux host or container with `perf` available to run against, and rather
than fabricate plausible-looking counter values, this section only shows
runs that were actually captured. Fill this section in with real `perf
stat` output the first time this script runs on Linux (CI, once wired up
per the recommendation above, or a local Linux box/container).
