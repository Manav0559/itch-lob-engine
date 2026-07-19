# itch-lob-engine

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
- [x] Execution strategies: Twap (time-sliced), Vwap (elapsed-time-weighted,
      tape-reactive), Pov (percentage-of-volume) — header-only, allocation-free,
      no floating point, share the same compile-time-dispatched
      `ExecutionStrategy` interface
- [x] `mmap` input path for large uncompressed day files; gzip-streaming input
      path (chunked `inflate`, carries partial frames across chunk boundaries)
      for compressed day files
- [x] Flat tick-ladder book (`LadderBook`), a drop-in alternative to `OrderBook`
      with the identical interface and correctness bar
- [x] `LadderBook` vs. `OrderBook` benchmarked head-to-head: `./build/bench`,
      p50/p99/p99.9 per message type, committed CSV + plots (see table below
      and [bench/](bench/))
- [x] Full-day invariant suite: a synthetic trading day replayed through
      `OrderBook`, `LadderBook`, and an independent from-scratch reference
      model, cross-checked against each other at every 250-message checkpoint
      (`tests/test_full_day_invariants.cpp`)
- [x] Execution fill simulation (`exec::FillSimulator`): scores Twap/Vwap/Pov's
      `ChildOrder`s against a replayed quote/tape (realized fill price, VWAP,
      fill rate) — see the scope notes in `include/exec/fill_sim.hpp` for what
      this lightweight model does and doesn't model (no resting/partial fills)

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
  | A    | 125 ns        | 42 ns          | 11,167 ns       | 1,375 ns         |
  | E    | 292 ns        | 167 ns         | 16,958 ns       | 2,459 ns         |
  | C    | 292 ns        | 167 ns         | 18,334 ns       | 3,417 ns         |
  | X    | 292 ns        | 167 ns         | 17,709 ns       | 2,792 ns         |
  | D    | 375 ns        | 250 ns         | 22,083 ns       | 5,666 ns         |
  | U    | 500 ns        | 333 ns         | 29,667 ns       | 8,000 ns         |

  Full distributions and plots: [bench/results.csv](bench/results.csv),
  [bench/plots/](bench/plots/). `std::map`'s tail is dominated by
  red-black-tree rebalancing on insert/erase; `LadderBook` pays a fixed
  array-index cost regardless of how full the book is, at the cost of a
  bounded price window fixed at construction (see `include/book/ladder_book.hpp`).

## Build & test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run the pipeline end-to-end without any data file:

```bash
./build/replay --selftest
```

## Replaying a real trading day

NASDAQ publishes free full-day sample files (several GB gzipped, ~13 GB
uncompressed — plan disk accordingly):
<https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/>

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

## License

MIT
