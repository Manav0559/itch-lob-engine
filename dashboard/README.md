# Results dashboard

A single static HTML page that renders the benchmark CSVs already committed
under [`bench/`](../bench/) — no Node/npm toolchain, no build step, no
external requests. It's a client-side chart, not a generator: it never
computes a number the repo doesn't already produce, it just parses and
draws what's in the CSVs.

## Run it

From the repo root:

```bash
python3 -m http.server
```

Then open **http://localhost:8000/dashboard/** in a browser.

That's it — no `pip install`, no dependencies. The page uses hand-rolled
inline SVG charts (no CDN scripts), so it also works fully offline.

Don't open `dashboard/index.html` directly as a `file://` URL — browsers
block `fetch()` against local files, so the CSVs won't load and the page
will show a banner explaining this. Serving it over `http://` (any local
static server, not just `python3 -m http.server`) is required.

## What it shows

1. **OrderBook vs LadderBook latency** — p50 / p99 / p99.9 nanoseconds per
   message type, from `bench/results.csv`, as three small-multiple grouped
   bar charts (one per percentile — the values span roughly two orders of
   magnitude, so a shared axis would flatten the small ones).
2. **Threaded vs single-threaded throughput** — messages/sec (median and
   best-of-3) per replay path from `bench/results_threaded.csv`, plus a
   table of the SPSC queue round-trip latencies from the same file.
3. **CI performance ratio over commits** — LadderBook/OrderBook p50 ratio,
   averaged across message types, per commit recorded to
   `bench/ci_history.csv` (faceted by OS). The CI workflow now calls
   `bench/check_budget.py record` on every push to main and commits the
   resulting row back automatically (see `.github/workflows/ci.yml` and
   `bench/CI_INTEGRATION.md`), so `bench/ci_history.csv` already has real
   rows and this section renders the chart directly. It still shows the
   empty-state message on a fresh checkout of a branch that hasn't had CI
   land on it yet, or if `bench/ci_history.csv` is ever reset back to just
   its header row — no dashboard changes are needed either way, it reads
   whatever rows are there.

## Refreshing the data

The dashboard doesn't run the benchmarks itself; it reads whatever the last
run wrote. Regenerate the source CSVs, then just reload the page:

```bash
cmake --build build --target bench && ./build/bench          # bench/results.csv
./build/bench_threaded                                       # bench/results_threaded.csv (manual target, see bench/THREADED_PIPELINE_FINDINGS.md)
python3 bench/check_budget.py record                         # appends a row to bench/ci_history.csv
```

## Notes on implementation

- Charts are hand-rolled inline SVG (grouped bar chart + line chart
  helpers in `index.html`), not a charting library — keeps the page
  dependency-free and CSP-safe with zero external requests.
- CSV parsing is a small hand-rolled splitter; it assumes the simple
  comma-separated, no-embedded-commas format the `bench/` CSVs actually use
  (matches `bench/plot.py`'s reader).
- Light/dark mode follow `prefers-color-scheme` automatically; the
  "Toggle theme" button overrides that and remembers the choice in
  `localStorage`.
- Every section fails independently and shows an inline empty/error state
  (missing file, empty file, unexpected shape) rather than breaking the
  whole page — useful before the first `./build/bench` run, or if only one
  of the three CSVs exists.
