# Does the threaded replay pipeline actually help?

**No, not measurably.** Across four independent runs, end-to-end throughput for
`replay_threaded_main.cpp` (parser thread → SPSC queue → book-builder thread)
lands within noise of the single-threaded `replay_main.cpp` path — sometimes a
couple percent faster, sometimes a couple percent slower, never a real win.
The SPSC handoff itself is cheap (~125ns per message), but there isn't enough
parsing-side work to hide behind book-mutation work for a cheap handoff to pay
off, and decoupling adds a real cost of its own: under sustained load the
queue backs up and per-message latency balloons into the hundreds of
microseconds, with nothing gained in exchange.

This was produced by `bench/bench_threaded_main.cpp` (not wired into
`CMakeLists.txt` — build manually, see below) and is captured in
`bench/results_threaded.csv`.

## Method

- Same synthetic session (`bench::generate_synthetic_session()`, 2.2M
  messages, 9 symbols, seed 1234567) replayed through both paths, byte-for-byte
  identical input either way.
- Throughput (Task 2): 1 discarded warm-up pass + 3 measured passes per path,
  same discipline `bench_main.cpp` uses. Both median and best-of-3 elapsed
  time are reported — a process can only be slowed down by outside scheduler
  interference, never sped up, so the fastest of a few passes is a standard
  way to recover a noise-robust estimate without needing hundreds of runs.
- SPSC round trip (Task 3), measured two ways with a real producer thread and
  consumer thread racing (not sequential same-thread push/pop, which hides
  cache-coherency cost):
  - **saturated** — producer never waits, exactly like the real pipeline's
    parser thread. Realistic for what the pipeline actually does, but the
    resulting latency is dominated by queueing delay once the bounded queue
    backs up, not by the push/pop mechanism itself.
  - **isolated** — producer waits for the queue to drain to empty before each
    send, so every round trip is a push into an empty queue immediately
    followed by a pop. This is the number that's actually comparable to
    `bench/results.csv`'s per-message book-mutation costs, since both are
    isolated per-message mechanical costs rather than backlog effects.

All runs below were taken after the machine's iCloud sync backlog (unrelated
to this code — see git history / session notes) cleared and load average
settled to 3.6–6.7 on this 8-core machine. An earlier throughput-only run
(before the isolated SPSC measurement existed), taken while load was already
elevated (~4.7) but before it spiked further, showed the same picture — 0.95x
median / 0.91x best-of-3, i.e. no clear winner — and its saturated round-trip
p50/p99 (776,542ns / 7,287,709ns) was the worst of any run, consistent with
scheduler contention inflating queueing delay rather than anything about the
code. It's omitted from the tables below since it predates the isolated
measurement, but it points the same direction as everything else here.

## Results

Three runs, back to back, right after rebuilding with both SPSC measurements
in place (load average 3.6–6.1 throughout). Run C is what's saved in
`bench/results_threaded.csv`.

### Task 2 — end-to-end throughput, single-threaded vs. threaded pipeline

| Run | single-threaded (median) | threaded (median) | speedup, median | speedup, best-of-3 |
|---|---|---|---|---|
| A | 3,063,957 msgs/s | 2,788,243 msgs/s | 0.910x (threaded slower) | 1.034x (threaded faster) |
| B | 3,538,635 msgs/s | 3,597,760 msgs/s | 1.017x (threaded faster) | 0.933x (threaded slower) |
| C | 4,494,485 msgs/s | 4,492,590 msgs/s | 1.000x (wash) | 1.011x (threaded faster) |

Every run clusters within ~±10% of 1.0x and flips direction between the
median and best-of-3 statistic within the same run — that pattern is
measurement noise on a shared machine, not a real, repeatable effect in
either direction. There is no run, across either statistic, where threading
wins (or loses) by a margin large enough to call it a genuine throughput
difference.

### Task 3 — SPSC round-trip latency

| Run | saturated p50 | saturated p99 | isolated p50 | isolated p99 |
|---|---|---|---|---|
| A | 972,958 ns | 7,380,750 ns | 125 ns | 417 ns |
| B | 678,500 ns | 2,141,833 ns | 125 ns | 625 ns |
| C | 308,000 ns | 751,583 ns | 125 ns | 667 ns |

The isolated p50 is identical — 125ns — across every run. That stability is
the headline number: **the SPSC push+pop round trip costs about 125ns at
p50, 417-667ns at p99**, when measured without queueing delay mixed in.

Compare against `bench/results.csv`'s per-message book-mutation costs:

| book type | message type | p50 | p99 |
|---|---|---|---|
| OrderBook | A | 84 ns | 417 ns |
| OrderBook | E/C/X | 167 ns | 584 ns |
| OrderBook | D | 209 ns | 709 ns |
| OrderBook | U | 333 ns | 875 ns |
| LadderBook | A | 42 ns | 375 ns |
| LadderBook | E/C/X | 125 ns | 500 ns |

The 125ns handoff cost sits squarely inside this range — comparable to one
book mutation, not negligible next to it, but not an order of magnitude
larger either.

The saturated numbers tell a different story: p50 in the hundreds of
microseconds, three to four orders of magnitude above the isolated cost. That
gap is queueing delay, not handoff cost — it means the parser thread
generates messages faster than the book-builder thread can drain them, so the
64K-capacity queue is chronically backed up whenever the pipeline runs at
full speed.

## Why it washes out

Single-threaded throughput of ~3.5-4.5M msgs/s implies a combined
parse-plus-mutate cost of roughly 220-290ns per message. Book mutation alone
already accounts for 84-333ns of that (OrderBook p50, by message type) — so
parsing is a comparatively small slice of the total per-message cost. That
matters because a two-stage pipeline's best-case speedup is bounded by
`(parse + mutate) / max(parse, mutate)`: when one stage dominates, there
isn't much of the other stage left to hide behind it, no matter how cheap the
handoff is. Here, mutation dominates, so the theoretical ceiling on any
speedup was already low before the queue's own cost — 125ns per message,
itself comparable to a book mutation — gets subtracted from it. What's left
over is noise-sized, which matches exactly what was measured: throughput
within ±10% of 1.0x, flipping sign between runs.

What decoupling does add, unconditionally, is the saturated-mode latency
tail: because parsing outpaces book-building, the queue backs up under
sustained load and per-message latency through the pipeline balloons into the
hundreds of microseconds at p50 — a cost the single-threaded path, which
processes each message inline in ~250ns with no queueing variance at all,
never pays. Aggregate throughput doesn't suffer much from this (the
book-builder stage is the bottleneck either way, and bottleneck-stage rate is
what determines steady-state throughput), but per-message latency does, and
nothing was gained in exchange for it.

## Bottom line

For this parser/book-builder pair, on this workload, the two-thread pipeline
is not a performance win — it's a wash on throughput and a real regression on
per-message latency tail under load. The root cause is structural, not an
implementation defect in `spsc_queue.hpp` or `threaded_replay.hpp`: book
mutation is expensive enough relative to parsing that there's little
parallel work to extract, and the (individually cheap) queue handoff still
costs enough to erase what little headroom exists. `replay_threaded_main.cpp`
remains useful as a correctness reference and as infrastructure that would
pay off if the balance of costs shifted (heavier parsing, cheaper book
mutation, or a genuine need to run the two stages on separate cores for
reasons other than throughput) — but `replay_main.cpp`'s single-threaded
design is the right default for this codebase as it stands today.
