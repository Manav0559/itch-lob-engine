# bench

Head-to-head harness for `book::OrderBook` (`std::map` ladders) vs
`book::LadderBook` (flat tick-indexed ladders): throughput and per-message
cost, measured in batches.

## Methodology

A single book update costs tens of nanoseconds. On Apple Silicon,
`std::chrono::steady_clock` is backed by the 24 MHz system counter, so one
tick is ~41.7 ns (`./build/bench` prints the resolution it observes).
Timing one call at a time with that clock can only return 0, 41.7, 83.3 ...
ns. The previous version of this harness did exactly that, and its
"LadderBook Add p50 = 42 ns" was one clock tick, not a measurement.

The harness now:

1. decodes the stream once into `pipeline::Envelope`s (so parsing is not
   timed), then replays them through the production
   `pipeline::BookBuilder` / `dispatch_to_book` path, which includes the
   per-symbol book lookup;
2. reads the clock once per **batch of 1024 messages** (`--batch=N`), so a
   clock tick is well under 0.1% of each timed interval;
3. reports, per book type, one `ALL` row with
   - `msgs_per_sec`: total messages / total timed time, and
   - `p50_ns`/`p99_ns`/`p999_ns`: percentiles over batches of
     (batch time / batch size). These are **amortized** per-message costs.
     A single slow message is averaged with the other 1023 in its batch, so
     this is not the tail latency of an individual message;
4. on x86-64 only, adds per-type rows (`method=rdtsc_per_msg`): one
   `lfence; rdtsc` / `rdtscp; lfence` pair per call, calibrated against
   `steady_clock`, with the median cost of an empty timer pair subtracted.
   arm64 has no user-space counter finer than the 24 MHz one on Apple
   M-series, so these rows are skipped there instead of being reported as
   tick multiples.

Each book type gets one discarded warm-up pass, then `--passes=N` (default
3) measured passes, each against fresh books.

## Running

```bash
cmake --build build --target bench
./build/bench                              # synthetic session (deterministic, no data file needed)
./build/bench --batch=4096 --passes=5      # change the batch size / measured passes
./build/bench --summary-md=summary.md      # also append a markdown table (CI job summary)
./build/bench /path/to/day.NASDAQ_ITCH50    # real, already-uncompressed file — mmap'd
./build/bench /path/to/day.NASDAQ_ITCH50.gz # real, gzipped file — streamed, never fully materialized
```

Getting a real day file onto disk: `bench/fetch_itch_day.sh <name>` (e.g.
`bench/fetch_itch_day.sh 12302019`) downloads it from NASDAQ's public
archive using many parallel byte-range requests rather than one plain
`curl -o file url` — see the script's header comment and
[docs/devlog-orderbook-vs-ladderbook.md](../docs/devlog-orderbook-vs-ladderbook.md#closing-the-open-question-a-real-nasdaq-day)
for why that matters: a single connection to that host measured well under
100 KB/s in one dev environment, turning a multi-GB file into a multi-hour
(or truncated, if the session ends first) download. The script verifies
the assembled file's size against the server's `Content-Length` and runs
`gzip -t` on it before reporting success.

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

For `.gz` input the stream cannot be decoded into memory first, so each
timed batch also includes parse and inflate time; the CSV's `method`
column says `batch1024_incl_parse_inflate` in that case.

A real file's numbers are never auto-committed over `bench/results.csv` —
that file stays the synthetic, reproducible-by-anyone baseline the README
quotes. `bench/results_real_day.csv` is an older real run against a full
NASDAQ day file, produced by the previous per-call harness on an Apple M2:
its values are multiples of the ~41.7 ns clock tick, so treat it as
historical and do not read its small values as latencies. It has a second
caveat too: see
[docs/devlog-orderbook-vs-ladderbook.md](../docs/devlog-orderbook-vs-ladderbook.md#closing-the-open-question-a-real-nasdaq-day)
before trusting `LadderBook`'s numbers in it — a fixed-window sizing issue
that file surfaced makes roughly 30% of `LadderBook`'s timed mutations on
it cheap rejections, not real work.

## Plotting

```bash
pip install matplotlib   # if not already available
python3 bench/plot.py
```

Reads `bench/results.csv` and regenerates `bench/plots/per_msg_ns.png`
(amortized per-message p50/p99/p99.9), `bench/plots/throughput.png`, and,
when the CSV has x86 per-type rows, `bench/plots/per_type_p50.png`. Only
plots whatever is in the CSV.

## CI

- `build-test` runs `./build/bench` and `bench/check_budget.py check` (the
  same-run LadderBook/OrderBook ratio gate, see [BUDGET.md](BUDGET.md)).
- `bench-summary` (ubuntu-latest, `continue-on-error`) runs the benchmark
  and writes the table to the job summary. GitHub-hosted runners are
  shared and virtualized, so compare the two books within one run, not
  absolute numbers across runs.
