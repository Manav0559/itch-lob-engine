# bench/check_budget.py — CI performance budget

`bench/bench_main.cpp` used to be something you ran by hand and eyeballed.
This turns it into an enforced gate: `bench/check_budget.py` reads
`bench/results.csv` (written by `./build/bench`) and decides whether the
numbers are acceptable.

## The problem this is designed around

CI runners — especially GitHub-hosted ones — are noisy, shared, virtualized
machines. Their absolute nanosecond numbers are not comparable to a
developer's laptop, and are not even stable run-to-run on the *same*
runner. A gate that fails when `LadderBook p99 > 900ns` will flake
constantly as soon as a neighboring VM steals a cache line or the hypervisor
schedules a noisy neighbor.

So there are two separate checks with two separate jobs:

## Check 1 — ratio gate (hard fail)

For every message type, `LadderBook`'s p50 must be under
`--ratio-threshold` (default **0.90**, i.e. LadderBook must be at least 10%
faster than OrderBook) of `OrderBook`'s p50, **on the same run**.

This is the check that is allowed to fail the build. It works because it's
a ratio computed within a single benchmark run on a single machine at a
single moment — whatever noise inflates `OrderBook`'s numbers on a given CI
run inflates `LadderBook`'s numbers too, so the ratio cancels most of that
noise out. What it can't cancel out is a real regression: e.g. someone adds
an allocation or a `std::map` lookup to `LadderBook`'s hot path and its
advantage over `OrderBook` narrows or disappears.

0.90 was picked as loose enough that normal CI jitter (which affects both
books roughly proportionally) won't trip it — current healthy ratios range
from ~0.34 to ~0.78 (see `bench/results.csv`), so there's real headroom —
but tight enough that a regression which makes LadderBook only marginally
faster than OrderBook (defeating the entire point of the flat-ladder
design) still fails.

## Check 2 — drift warning (soft, informational)

Separately, `bench/ci_history.csv` is a rolling, append-only log of every
CI run's numbers on `main`, one row per `(commit, os, book_type,
message_type)`. Check 2 compares this run's p99 against the median of its
own last `--history-window` (default 10) entries for the *same OS label*
(Linux and macOS runners have different absolute performance profiles, so
they're never compared to each other). If the current p99 exceeds
`--drift-multiplier` (default 1.75x) that median, it **prints a warning and
exits 0** — it never fails the build. This is deliberately loose and
deliberately non-blocking: absolute ns numbers on a shared runner are too
noisy to gate on directly, but tracking them over time is still useful for
a human to notice "huh, LadderBook's p99 has been creeping up for two
weeks" before it becomes a real problem.

If there are fewer than `--min-history` (default 3) prior entries for a
given `(os, book_type, message_type)`, that combination is skipped rather
than warned on — there's nothing to compare against yet.

## File layout

- `bench/results.csv` — this run's raw output, overwritten every time
  `./build/bench` runs. Not versioned as history; it's just the latest
  measurement.
- `bench/ci_history.csv` — committed, append-only. Columns:
  `timestamp_utc,commit_sha,os,book_type,message_type,p50_ns,p99_ns,p999_ns`.
  Starts as a header-only file; real rows accumulate automatically — every
  push to `main` that clears the budget check runs `check_budget.py record`
  and the CI job commits the result straight back to `main` with the default
  `GITHUB_TOKEN` (see `bench/CI_INTEGRATION.md` for the exact workflow steps).
  The `os` column holds GitHub Actions' `RUNNER_OS` value (`Linux` /
  `macOS`) when run in CI, or `platform.system()`'s output (e.g. `Darwin`)
  when run locally — local runs are for testing the script, not for seeding
  real CI history, since the OS labels won't match.

## Usage

```bash
# Hard gate + soft warning, against the results.csv just written by ./build/bench:
python3 bench/check_budget.py check

# Append this run's numbers to the rolling history (CI already does this
# automatically on every push to main that clears the budget check — see
# bench/CI_INTEGRATION.md; run it by hand only for local testing or to
# backfill a run CI didn't record):
python3 bench/check_budget.py record

# Useful flags:
python3 bench/check_budget.py check --ratio-threshold 0.85
python3 bench/check_budget.py check --drift-multiplier 2.0 --history-window 20
python3 bench/check_budget.py --results other.csv --history other_history.csv check
```

`record` is idempotent per `(commit_sha, os)` — re-running it for a commit
that's already in the history (e.g. a re-triggered CI job) is a no-op
rather than a duplicate row.

## Testing this script

```bash
# 1. Build and run the real benchmark, confirm the gate passes on healthy numbers:
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target bench
./build/bench
python3 bench/check_budget.py check   # expect exit 0

# 2. Hand-construct a regressed results.csv (inflate LadderBook's p50 past
#    90% of OrderBook's) and confirm the gate actually fails:
cp bench/results.csv /tmp/regressed.csv
#   ...edit LadderBook,A's p50_ns to something >= 0.90 * OrderBook,A's p50_ns...
python3 bench/check_budget.py --results /tmp/regressed.csv check   # expect exit 1
```

A budget check that has never been proven to fail is not a real gate — both
directions were exercised during development of this script.
