# bench

Head-to-head latency harness for `book::OrderBook` (`std::map` ladders) vs
`book::LadderBook` (flat tick-indexed ladders), timed per message type.

## Running

```bash
cmake --build build --target bench
./build/bench                              # synthetic session (deterministic, no data file needed)
./build/bench /path/to/day.NASDAQ_ITCH50    # real, already-uncompressed file — mmap'd
./build/bench /path/to/day.NASDAQ_ITCH50.gz # real, gzipped file — streamed, never fully materialized
```

Run from the repo root — it writes `bench/results.csv` relative to the
current directory.

Absent a file argument, `bench_main` generates a fixed-seed synthetic
session (2.2M+ messages across 9 symbols, realistic message-type mix) using
the same mirror encoders the unit tests and `replay --selftest` use, so the
numbers below are reproducible by anyone who clones the repo without needing
an exchange data file.

For a real file, `.gz` inputs are re-read and re-inflated from disk once per
pass (same bounded, few-MB-at-a-time chunked `inflate` as `replay`'s gzip
path) rather than decompressed once and replayed from memory: a real NASDAQ
day can run to multi-GB uncompressed, well past what fits comfortably
alongside everything else on a memory-constrained dev machine. An
already-uncompressed real file is still mmap'd directly, same as before.

Each book type gets one discarded warm-up pass followed by three measured
passes against fresh books; percentiles are computed over the combined
measured samples per message type.

## Plotting

```bash
pip install matplotlib   # if not already available
python3 bench/plot.py
```

Reads `bench/results.csv` and regenerates `bench/plots/*.png` (p50/p99/p99.9
bar charts, OrderBook vs LadderBook, grouped by message type). Only plots
whatever is in the CSV — never fabricates numbers.
