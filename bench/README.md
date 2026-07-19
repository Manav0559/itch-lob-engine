# bench

Head-to-head latency harness for `book::OrderBook` (`std::map` ladders) vs
`book::LadderBook` (flat tick-indexed ladders), timed per message type.

## Running

```bash
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -Iinclude bench/bench_main.cpp -o /tmp/bench_main
/tmp/bench_main               # synthetic session (deterministic, no data file needed)
/tmp/bench_main /path/to/day  # real NASDAQ ITCH file instead, mmap'd
```

Run from the repo root — it writes `bench/results.csv` relative to the
current directory.

Absent a file argument, `bench_main` generates a fixed-seed synthetic
session (2.2M+ messages across 9 symbols, realistic message-type mix) using
the same mirror encoders the unit tests and `replay --selftest` use, so the
numbers below are reproducible by anyone who clones the repo without needing
an exchange data file. Pass a real day file (as accepted by `replay`, mmap'd
the same way) to benchmark against actual data instead.

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
