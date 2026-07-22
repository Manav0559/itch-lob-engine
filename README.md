# itch-lob-engine

[![CI](https://github.com/Manav0559/itch-lob-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/Manav0559/itch-lob-engine/actions/workflows/ci.yml)
[![Coverage](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/Manav0559/itch-lob-engine/main/coverage/coverage.json)](https://github.com/Manav0559/itch-lob-engine/actions/workflows/ci.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A NASDAQ TotalView-ITCH 5.0 order-book reconstruction engine in C++20.

Parses the real exchange binary protocol (length-prefixed `BinaryFILE`
framing, big-endian wire integers, 48-bit nanosecond timestamps), rebuilds the
full limit order book per symbol from individual order events
(add / execute / cancel / delete / replace), and replays complete trading
days.

**Project rule: every number in this README is produced by a committed target
you can run.** The numbers below come from `./build/bench` (see
[bench/](bench/)) on an Apple M2 (arm64, macOS 26.1), `-O2`, AppleClang 17 —
a fixed-seed synthetic 2.2M-message session across 9 symbols, one discarded
warm-up pass + 3 measured passes. Regenerate with:

```bash
cmake --build build --target bench && ./build/bench && python3 bench/plot.py
```

## Architecture

[docs/architecture.md](docs/architecture.md) — the two entry points (file
replay vs. live UDP multicast), the shared `BookBuilder`/`BookTable`
abstraction both replay binaries route through, the `OrderBook`/`LadderBook`
swap point, and where the exec/risk-gate/fill-sim layer sits relative to the
book — one page, diagrammed, meant to be read before anything else in this
repo.

## Status

- [x] ITCH 5.0 stream framing (2-byte length prefix; unknown/corrupt frames
      skipped by length — the stream can never desynchronize)
- [x] Decoders for the book-building set: `A F E C X D U`, byte-offset exact,
      round-trip tested against mirror encoders
- [x] Order-id book: hash-map locator + price-level aggregates, `std::map`
      ladders (deliberate v1 baseline)
- [x] Unit tests (Catch2) incl. the classic footguns: phantom empty levels,
      duplicate refs, over-executes on feed gaps, truncated tails
- [x] CI on Linux + macOS, `-Wall -Wextra -Wpedantic -Werror`
- [x] Execution strategies: Twap (time-sliced), Vwap (tape-reactive,
      scheduled against a fixed-bucket historical intraday volume-curve model
      — the classic U-shape, heavy at the open/close, light mid-day — instead
      of a flat elapsed-time ramp; see `include/exec/volume_curve.hpp`), Pov
      (percentage-of-volume) — header-only, allocation-free, share the same
      compile-time-dispatched `ExecutionStrategy` interface. No floating
      point on the per-message hot path anywhere in the three; Vwap's one
      genuine use of it (turning the curve's hand-authored weights into an
      integer lookup table) is confined to construction, not the hot path.
- [x] `mmap` input path for large uncompressed day files; gzip-streaming input
      path (chunked `inflate`, carries partial frames across chunk boundaries)
      for compressed day files
- [x] Flat tick-ladder book (`LadderBook`), a drop-in alternative to `OrderBook`
      with the identical interface and correctness bar
- [x] `LadderBook` vs. `OrderBook` benchmarked head-to-head: `./build/bench`,
      p50/p99/p99.9 per message type, committed CSV + plots (see table below
      and [bench/](bench/))
- [x] **`LadderBook` is the production default**, not just a benchmark
      exhibit: `replay`/`replay_threaded` route every message through a
      dense, locate-indexed `pipeline::BookTable` (`include/pipeline/
      book_table.hpp`) backed by `LadderBook` by default — pass `--map` to
      force the `std::map`-based `OrderBook` instead, for an explicit A/B.
      Wiring this up surfaced (and fixed) a real bug: `LadderBook`'s tick-grid
      alignment wasn't anchored consistently, which would have silently
      merged distinct real price levels once fed actual exchange data instead
      of grid-perfect synthetic prices — see
      [docs/devlog-orderbook-vs-ladderbook.md](docs/devlog-orderbook-vs-ladderbook.md).
- [x] Validated against a real, complete NASDAQ day file (not just the
      synthetic session): `./build/bench` and `./build/replay` against the
      full public Dec 30, 2019 file (268.7M frames, 8,892 symbols) confirm
      `LadderBook` stays faster than `OrderBook` at every percentile, but
      also surfaced a second real bug in the same family as the one above —
      `LadderBook`'s fixed ±30% construction-time price window rejects
      ~30% of mutations on a real day's worth of real price action, which
      OrderBook's unbounded ladders never do — see
      [docs/devlog-orderbook-vs-ladderbook.md](docs/devlog-orderbook-vs-ladderbook.md#closing-the-open-question-a-real-nasdaq-day)
      for the numbers, the root cause, and what fixing it requires before
      the real-day results can replace the synthetic ones above as the
      primary claim.
- [x] Full-day invariant suite: a synthetic trading day replayed through
      `OrderBook`, `LadderBook`, and an independent from-scratch reference
      model, cross-checked against each other at every 250-message checkpoint
      (`tests/test_full_day_invariants.cpp`)
- [x] Execution fill simulation (`exec::FillSimulator`): scores Twap/Vwap/Pov's
      `ChildOrder`s against a replayed quote/tape (realized fill price, VWAP,
      fill rate) — see the scope notes in `include/exec/fill_sim.hpp` for what
      this lightweight model does and doesn't model (no resting/partial fills)
- [x] Multi-threaded pipeline (`replay_threaded`): parsing and book-building
      decoupled onto separate threads joined by a lock-free SPSC queue
      (`include/pipeline/spsc_queue.hpp`), instead of one thread doing both —
      produces identical book state to `replay` (now checked against both
      `OrderBook` and `LadderBook` — `tests/test_replay_threaded.cpp`), and
      reports max queue occupancy as a backpressure indicator.
      **Original finding** (measured against `OrderBook`, before `LadderBook`
      became the default): throughput within noise of single-threaded
      (0.75–1.03x across runs) and a real latency regression under load — see
      [bench/THREADED_PIPELINE_FINDINGS.md](bench/THREADED_PIPELINE_FINDINGS.md).
      **Open question, not yet resolved**: now that `LadderBook` (much
      cheaper per-message book mutation) is what both paths default to, a
      fresh `./build/bench_threaded` run showed the threaded pipeline
      *ahead* of single-threaded (~1.03–1.2x depending on pass) — the
      opposite of the original verdict. That single noisy run (this machine
      runs benchmarks alongside real background load) is not enough to
      overturn a documented conclusion; it needs the same rigor the original
      finding got (multiple runs, ideally with CPU core pinning — see the
      roadmap) before `THREADED_PIPELINE_FINDINGS.md` gets rewritten.
- [x] Live UDP multicast feed handler (`live_replay` + `multicast_sender`)
      running NASDAQ's real MoldUDP64 session protocol (session header,
      sequence numbers, sequence-gap detection, and a retransmission-request
      gap-fill round trip) — see `include/net/moldudp64.hpp` and
      `include/net/multicast_receiver.hpp` (`MoldUdp64Receiver`) for the
      implementation and its documented simplifications versus the full spec
- [x] Almgren-Chriss optimal-execution strategy (`include/exec/almgren_chriss.hpp`):
      a risk-averse, front-loaded trade trajectory alongside Twap/Vwap/Pov's
      simpler schedules
- [x] CI-enforced performance budget (`bench/check_budget.py`): a same-run
      ratio gate (LadderBook must stay meaningfully faster than OrderBook)
      that's robust to noisy CI machines, plus an informational drift warning
      against rolling CI history — see `bench/BUDGET.md`
- [x] Cross-platform hardware-counter profiling (`bench/hw_profile.sh`):
      `perf stat` on Linux, an honest smaller subset (`/usr/bin/time -l`) on
      macOS where userspace PMU access isn't available — see
      `bench/HARDWARE_PROFILING.md`
- [x] [Devlog: OrderBook vs. LadderBook](docs/devlog-orderbook-vs-ladderbook.md) —
      the tree-vs-array tradeoff, written up with the real measured numbers
- [x] Pre-trade risk gate (`include/exec/risk_gate.hpp`): per-order size /
      notional / price-collar limits plus a latching cumulative kill switch
      (requires an explicit `reset()` — no automatic self-healing) between a
      strategy's `ChildOrder` output and wherever orders go next
- [x] Execution layer wired end to end (`replay_exec`): a live book's quotes
      and prints drive Twap/Vwap/Pov/Almgren-Chriss directly — every mutation
      of one traded `--locate` publishes an `exec::Bbo` (and, on an execute,
      an `exec::TradeTick`) to the strategy, whose `ChildOrder`s are drained
      through `RiskGate` and scored by `FillSimulator`, all off the real
      parsed ITCH stream instead of hand-built test structs — see
      `include/exec/replay_exec_handler.hpp` and
      [docs/architecture.md](docs/architecture.md)'s exec-layer section
- [x] Coverage-guided fuzzing of the ITCH parser (`fuzz/`): libFuzzer +
      ASan/UBSan against `itch::parse_stream` through a real `BookBuilder`
      (`LadderBook`-backed, the same default production now uses), not
      just decode-in-isolation — 4.6M+ executions across seed runs, clean, no
      crashes found so far; see [fuzz/README.md](fuzz/README.md)
- [x] Measured line/branch coverage, not just a test count: a dedicated CI
      job builds `include/` + `src/` with `--coverage` (gcc), runs the full
      112-test suite, and captures/filters the result with `lcov` — the
      README badge above and every CI run's job summary report an actual
      percentage instead of asserting thoroughness in prose; see
      [coverage/README.md](coverage/README.md) for what's measured (and
      deliberately *not* gated on, and why) and how to reproduce it locally.
- [x] Live read-only query service (`replay_query`): a TCP JSON-lines server
      (`include/net/query_server.hpp`) answering best bid/ask, depth, and
      open-order-count questions per stock-locate while a replay runs —
      `{"cmd":"list"}` / `{"cmd":"quote","locate":N}` in, one JSON object per
      line out. The ingest side (parsing + book mutation) never talks to the
      query server directly: it periodically publishes a plain-value
      snapshot of the `BookTable` into a mutex-guarded `SnapshotStore`
      (`include/pipeline/book_snapshot.hpp`), and every query thread reads
      only that snapshot — the lock is held only for the snapshot handoff
      itself (microseconds), never for a book mutation or for the O(symbol
      count) walk that builds it, so the per-message hot path is completely
      untouched. See `include/pipeline/book_snapshot.hpp`'s header comment
      for the full concurrency-boundary rationale, and the "Other binaries"
      section below for usage.
- [x] Dependency-light static results dashboard (`dashboard/index.html`):
      client-side `fetch()` of the committed `bench/*.csv` files, hand-rolled
      inline-SVG charts (no Node/npm, no CDN, no build step) — OrderBook vs.
      LadderBook latency, threaded vs. single-threaded throughput, and the CI
      performance-ratio history against the `bench/BUDGET.md` threshold, with
      light/dark themes and an empty-state per section if a CSV is missing;
      see [dashboard/README.md](dashboard/README.md) for how to view it
      (needs a local HTTP server — `python3 -m http.server` — since browsers
      block `fetch()` against `file://`)

## Design notes

- **Framing before parsing.** The walker trusts only the 2-byte length
  prefix, so message types this engine doesn't decode (or future spec
  additions) are skipped, not tripped over. A known type arriving with the
  wrong length is surfaced as corrupt — never misparsed.
- **Locator + aggregates.** ITCH executes/cancels/deletes reference the order
  id, never the price. Per-order state therefore lives in an
  `unordered_map<ref, {shares, price, side}>`; the ladders only carry
  per-level `{shares, order count}`. Empty levels are erased immediately —
  a phantom level corrupts best-bid/ask and depth, and there is a regression
  test for exactly that.
- **`std::map` first, on purpose.** The interesting claim was never "arrays
  are faster than red-black trees" in the abstract — it's *by how much*,
  measured on the same byte-identical stream through both implementations.
  `LadderBook`'s O(1) indexed lookup beats `OrderBook`'s O(log levels) tree
  work across every message type, and the gap widens sharply in the tail:

  | type | OrderBook p50 | LadderBook p50 | OrderBook p99.9 | LadderBook p99.9 |
  |------|--------------:|---------------:|----------------:|-----------------:|
  | A    | 166 ns        | 42 ns          | 10,709 ns       | 4,125 ns         |
  | E    | 334 ns        | 208 ns         | 13,166 ns       | 7,625 ns         |
  | C    | 334 ns        | 208 ns         | 13,500 ns       | 7,666 ns         |
  | X    | 334 ns        | 208 ns         | 13,792 ns       | 7,167 ns         |
  | D    | 417 ns        | 292 ns         | 15,667 ns       | 10,375 ns        |
  | U    | 542 ns        | 375 ns         | 22,458 ns       | 12,042 ns        |

  ![p50 latency, OrderBook vs LadderBook](bench/plots/p50_ns.png)
  ![p99 latency, OrderBook vs LadderBook](bench/plots/p99_ns.png)
  ![p99.9 latency, OrderBook vs LadderBook](bench/plots/p999_ns.png)

  Full distributions and plots: [bench/results.csv](bench/results.csv),
  [bench/plots/](bench/plots/) (regenerate with `python3 bench/plot.py`
  after any `./build/bench` run — requires `pip install matplotlib`).
  `std::map`'s tail is dominated by
  red-black-tree rebalancing on insert/erase; `LadderBook` pays a fixed
  array-index cost regardless of how full the book is, at the cost of a
  bounded price window fixed at construction (see `include/book/ladder_book.hpp`).
  Regenerated after `LadderBook` became the production default (see the
  `pipeline::BookTable`/`BookBuilder` entry above) — absolute numbers move
  run to run with this machine's background load (the project's own
  single-run-no-warmup caveat, and why `bench/check_budget.py`'s CI gate is a
  same-run ratio check, not an absolute one), but the ratio between the two
  books is the reproducible, load-bearing part of this table.

## Build & test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Line/branch coverage (what CI's `coverage` job and the badge above measure):

```bash
cmake -B build-coverage -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="--coverage -O0" -DCMAKE_EXE_LINKER_FLAGS="--coverage"
cmake --build build-coverage --parallel
ctest --test-dir build-coverage --output-on-failure
lcov --capture --directory build-coverage --output-file coverage/lcov.info --rc branch_coverage=1
lcov --remove coverage/lcov.info '/usr/*' '*/_deps/*' '*/tests/*' \
  --output-file coverage/lcov.filtered.info --rc branch_coverage=1
lcov --list coverage/lcov.filtered.info --rc branch_coverage=1
```

See [coverage/README.md](coverage/README.md) for what's included/excluded,
why there's no coverage-percentage gate, and how the README badge gets its
number.

Run the pipeline end-to-end without any data file:

```bash
./build/replay --selftest
```

## Replaying a real trading day

NASDAQ publishes free full-day sample files (several GB gzipped, ~13 GB
uncompressed — plan disk accordingly):
<https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/>

A plain `curl -o file url` against that host can be surprisingly slow
depending on your network path — well under 100 KB/s was measured in one
dev environment, which turns a multi-GB file into a multi-hour download and
risks exactly the failure mode `./build/replay` is built to catch: a
download that never finishes leaves a truncated `.gz` that `gzip -t`
rejects with "unexpected end of file" and `./build/replay` correctly
refuses to parse ("gzip stream truncated before end marker") rather than
silently running on a partial day. `bench/fetch_itch_day.sh <name>` (e.g.
`bench/fetch_itch_day.sh 12302019`) fetches the same file via many
parallel byte-range requests instead, and verifies size + `gzip -t` before
declaring success — see [bench/README.md](bench/README.md) and
[docs/devlog-orderbook-vs-ladderbook.md](docs/devlog-orderbook-vs-ladderbook.md#closing-the-open-question-a-real-nasdaq-day)
for the full story, including what running against a complete real day
file actually surfaced.

```bash
./build/replay 12302019.NASDAQ_ITCH50.gz   # streams the gzip directly, chunk by chunk
./build/replay 12302019.NASDAQ_ITCH50      # mmaps an already-uncompressed file
```

Either path avoids materializing the full day in memory before parsing: `.gz`
files are inflated in fixed-size chunks and dispatched to the parser as they
decompress, and `.NASDAQ_ITCH50` files are mapped read-only rather than read
into a heap buffer. `./build/replay --legacy <file>` keeps the original
whole-file-into-memory path for A/B comparison on small files.

The replay reports frames parsed, per-type counts, books built, open orders,
and unknown-ref counts (which should be zero on an intact file from the start
of day).

## Other binaries

```bash
./build/replay_threaded --selftest   # same replay, parser + book-builder on separate threads
```

`replay_threaded` accepts the same `<file>` / `--legacy <file>` / `--selftest`
arguments as `replay` and produces identical book state — it's a comparison
binary for the decoupled-pipeline design, not a replacement, and additionally
reports max SPSC queue occupancy as a backpressure indicator.

```bash
./build/live_replay 239.255.0.1 12345 12346 5 &      # join a MoldUDP64 session, report every 5 frames
./build/multicast_sender 239.255.0.1 12345 12346     # send a synthetic session to it
```

Real ITCH is distributed over UDP multicast wrapped in NASDAQ's MoldUDP64
session protocol, and this is a real (if scoped) implementation of it, not a
raw-framing stand-in: every packet on the data channel carries a MoldUDP64
session header (10-byte session id + 8-byte sequence number + 2-byte message
count), `live_replay`'s `MoldUdp64Receiver` detects sequence gaps, and closes
them with a retransmission request/reply round trip on a separate request
channel that `multicast_sender`'s `MoldUdp64Sender` honors by replaying the
missed sequence range — see `include/net/moldudp64.hpp` and
`include/net/multicast_receiver.hpp` for the wire format and session-layer
implementation, and `tests/test_moldudp64.cpp` for gap-detection/gap-fill
tests against a simulated lossy sender.

Documented simplifications versus the full spec: one message per packet
rather than batching several into a message-count > 1 packet (the batched
case is decoded fine — `moldudp64.hpp`'s header format doesn't hardcode
count=1 — `MoldUdp64Sender` just never emits it); a bounded number of
gap-fill request/reply round trips before giving up on a given gap, with no
snapshot/refresh fallback after that point; and `multicast_sender`, being a
one-shot demo process rather than a persistent session server, only serves
retransmission requests for a few seconds after sending before it exits.
`live_replay` runs until interrupted (Ctrl-C / `SIGINT`) or a MoldUDP64
end-of-session packet, since a live feed otherwise has no natural end.

```bash
./build/replay_query --selftest --port 12401 &
printf '{"cmd":"list"}\n' | nc 127.0.0.1 12401
printf '{"cmd":"quote","locate":1}\n' | nc 127.0.0.1 12401
```

`replay_query` runs the same single-threaded replay as `replay` (same
`<file>` / `--legacy <file>` / `--selftest` / `--map` arguments), plus a
live, read-only TCP JSON-lines query server answering best bid/ask, depth,
and open-order-count questions per stock-locate — this is the one binary in
the repo with a request/response API surface rather than raw ingest/CLI
output. One JSON object per line in, one back:

```
{"cmd":"list"}                    -> every locate currently known
{"cmd":"quote","locate":1}        -> best bid/ask, depth, open orders for locate 1
```

```json
{"locate":1,"best_bid":null,"best_bid_shares":null,"best_ask":1500100,
 "best_ask_shares":400,"open_orders":1,"bid_levels":0,"ask_levels":1,
 "snapshot_version":1}
```

`best_bid`/`best_ask` are `null` (not `0`) when that side of the book is
empty, and `snapshot_version` counts how many times the query server's data
has been refreshed from the live book, so a caller can tell "no data yet"
apart from "data as of refresh #N." Additional flags: `--port N` (default
12401; `0` picks a free port, printed once bound), `--publish-every N`
(refresh the query server's data every N book-touching messages, default
2000), `--pace-us N` (sleep after each refresh — slows a small/`--selftest`
replay down enough to demo live querying against it), `--serve-seconds N`
(keep serving N seconds after replay finishes, then exit — default 0 serves
until Ctrl-C, same convention as `live_replay`).

By design this is a read-only diagnostic surface, not a general-purpose
service: no authentication, binds loopback-only, and `include/net/
query_server.hpp`'s request parser is a narrow hand-rolled scanner over this
one fixed schema, not a spec-compliant JSON parser (documented in that
file's header comment, the same way `include/net/multicast_receiver.hpp`
documents `MoldUdp64Receiver`'s simplifications versus the full MoldUDP64
spec). The concurrency boundary that makes this safe — the ingest side never
blocks on, or races with, a query thread, and neither ever touches a book
mutation lock — is `include/pipeline/book_snapshot.hpp`'s `SnapshotStore`;
see its header comment for the full design.

```bash
./build/replay_exec --selftest --strategy vwap
```

```
bytes            282
frames           9
elapsed          0.000 s
locate traded    1
child orders     attempted=1 accepted=1
risk gate        ok
fills            47 / 47 shares (100.0% fill rate)
fill vwap        150.0000
```

`replay_exec` runs the same single-threaded replay as `replay` (same
`<file>` / `--selftest` / `--map` arguments), but instead of just building
the book, wires one live `--strategy twap|vwap|pov|almgren_chriss` to it:
every mutation of `--locate`'s book (default `1`) publishes a fresh
`exec::Bbo` to the strategy, every execute against that locate reconstructs
an `exec::TradeTick`, and whatever the strategy pushes into its
`ChildOrderQueue` is drained through `exec::RiskGate` (generous, hardcoded
per-run limits — this binary demonstrates the pipeline, it isn't a risk
console) and scored by `exec::FillSimulator`. `--shares`, `--side`,
`--start-ts`/`--end-ts`/`--bin-ns`, `--participation-bps` (Pov),
`--risk-aversion`/`--volatility`/`--impact-coefficient` (Almgren-Chriss),
and `--limit-price` (Market child orders by default) tune the run; see
`./build/replay_exec` with no arguments for the full flag reference. The
glue itself — `include/exec/replay_exec_handler.hpp` — is header-only and
directly test-exercised (`tests/test_replay_exec.cpp`), not just reachable
by running the compiled binary; see
[docs/architecture.md](docs/architecture.md)'s exec-layer section for how
it fits against `replay`/`replay_threaded`/`live_replay`.

## License

MIT
