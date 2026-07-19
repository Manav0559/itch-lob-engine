# OrderBook vs. LadderBook: measuring the tree-vs-array tradeoff instead of asserting it

`itch-lob-engine` ships two order-book implementations with the identical
interface and the identical correctness bar: `OrderBook`, which keeps its bid
and ask ladders in `std::map`, and `LadderBook`, which keeps them in flat,
tick-indexed `std::vector`s. This is the story of why both exist, what the
second one actually costs you, and what the numbers say once you stop
asserting and start measuring.

## Why `std::map` shipped first, on purpose

`OrderBook` was the v1 baseline, and it stayed `std::map`-based on purpose,
not out of inertia. The claim "arrays are faster than red-black trees for
price ladders" is not interesting in the abstract — everyone already believes
some version of it. The interesting question is *by how much*, on the same
byte-identical message stream, through two implementations that share
everything except the ladder's data structure: same hash-map order locator,
same mutator semantics, same empty-level cleanup rules. That symmetry is what
makes the comparison a measurement rather than a guess. `LadderBook` was
built specifically so that comparison could happen, and `bench/` exists to
run it head-to-head rather than let either implementation's design notes make
the claim unchallenged.

## What LadderBook actually gives up

`LadderBook` is not a strictly-better replacement for `OrderBook` — it makes
a real tradeoff to get O(1) indexed lookups instead of O(log levels) tree
work. The ladder's price range is fixed at construction: you hand it a base
price and a window (defaults to ±20% around that price), and it allocates
`(max_price - min_price) / tick_size + 1` levels up front for each side.
`add()` on a price outside that window fails the same way it fails on a
duplicate order ref — a clean `false`, no partial state — but there is no way
to widen the window after construction. A name that gaps hard or halts and
reopens far from where it started needs a wider `window_pct` passed in up
front, or `LadderBook` simply won't hold it. `std::map`'s ladders have no
such ceiling; that unbounded range is the entire reason `OrderBook` stays the
baseline rather than getting replaced outright.

## What the benchmark actually shows

`bench/results.csv` comes from `./build/bench` run against a fixed-seed
synthetic 2.2M-message session (9 symbols, one discarded warm-up pass, three
measured passes) — the same numbers committed in the top-level README. Two
message types make the tree-vs-array story clearest:

| type | OrderBook p50 | LadderBook p50 | OrderBook p99.9 | LadderBook p99.9 | p50 gap | p99.9 gap |
|------|--------------:|---------------:|-----------------:|-----------------:|--------:|----------:|
| A    | 84 ns         | 42 ns          | 1,208 ns         | 625 ns           | 42 ns   | 583 ns    |
| U    | 333 ns        | 250 ns         | 2,666 ns         | 1,459 ns         | 83 ns   | 1,207 ns  |

For adds ('A'), LadderBook is already 2x faster at the median, and the gap
between the two implementations grows almost 14x in absolute terms by
p99.9. For replaces ('U') the median gap looks modest — LadderBook is only
about 1.3x faster at p50 — but the tail gap balloons to over 1.2 microseconds,
the widest of any message type in the CSV. That's not a coincidence: `'U'` is
implemented as a full `remove()` followed by a full `add()` in both books, so
it's doing two ladder mutations instead of one. For `OrderBook`, that's two
chances per message to trigger a red-black-tree rebalance; `std::map` insert
and erase are amortized O(log n), but the rebalancing work on any single call
is worst-case-driven, not average-case-driven, and worst cases compound. For
`LadderBook`, two array-index writes cost the same regardless of how many
levels are occupied or how the tree happens to be shaped at that instant. The
same pattern holds across every message type in the CSV — see
[bench/results.csv](../bench/results.csv) and the bar charts in
[bench/plots/](../bench/plots/) for the full distribution, not just the two
rows above.

## How you'd know if this was wrong

A benchmark that only tells you one implementation is faster is half the
story if you can't also trust it's correct — a faster book that quietly
drops levels or misreports best bid/ask isn't a win. That's what
`tests/test_full_day_invariants.cpp` is for: it replays a synthetic trading
day (six symbols, 2,000 events each) through `OrderBook`, `LadderBook`, and a
third, deliberately naive reference model built independently in the same
file — no aggregates, no incremental bookkeeping, best bid/ask recomputed by
scanning every live order on demand. All three get the exact same event
sequence, and every 250 messages the suite checks all three agree on best
bid, best ask, open order count, and level counts on both sides. Three
independent implementations of the same book-building rules agreeing at
every checkpoint is a much stronger signal than any one of them asserting on
itself. It's what makes trusting `LadderBook`'s numbers in the table above
reasonable in the first place — it's fast *and* independently verified
correct, not just fast.

## What this doesn't tell you yet

The benchmark has only run against a synthetic session so far, and that's
worth saying plainly rather than glossing over. The synthetic generator uses
fixed message-type weights and draws prices from stable per-symbol bands —
it doesn't reproduce the bursty, clustered order flow real exchanges see
around the open and close, when message rates and cancel/replace churn both
spike well past a steady-state average. Whether the tail gap between
`OrderBook` and `LadderBook` holds, narrows, or widens under that kind of
burst is an open question until this same harness runs against a real
NASDAQ ITCH day file, which `./build/bench /path/to/day.NASDAQ_ITCH50` already
supports.
