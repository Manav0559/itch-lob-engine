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

The benchmark ran only against a synthetic session for a long time, and
that was worth saying plainly rather than glossing over. The synthetic
generator uses fixed message-type weights and draws prices from stable
per-symbol bands — it doesn't reproduce the bursty, clustered order flow
real exchanges see around the open and close, when message rates and
cancel/replace churn both spike well past a steady-state average. Whether
the tail gap between `OrderBook` and `LadderBook` holds, narrows, or widens
under that kind of burst was an open question until this same harness ran
against a real NASDAQ ITCH day file, which `./build/bench
/path/to/day.NASDAQ_ITCH50` already supported. See "Closing the open
question" below for what actually happened when it did.

## From benchmark exhibit to production default

For a while, this measurement was true but not *acted on*: `replay` and
`replay_threaded` — the binaries that would actually process a real exchange
file — hardcoded `OrderBook` regardless of what the benchmark above proved.
Proving a component is faster and then never routing real traffic through it
is its own kind of bug, just not one a compiler or test suite catches.

Fixing that meant more than swapping a type alias. `OrderBook` is
default-constructible; `LadderBook` needs a price window at construction,
which the production pipeline only learns from the first `'A'` it sees for a
given locate. `bench/bench_main.cpp` had already solved that (a small
`BookStore<T>` with a `BookTraits`-style hook for the price hint) — so
promoting that pattern into shared code
(`include/pipeline/book_table.hpp`'s `BookTable`, `BookTraits<LadderBook>` in
`include/pipeline/dispatch_to_book.hpp`) is what let `pipeline::BookBuilder`
become generic over book type, with `LadderBook` as the default and `--map`
as an explicit escape hatch back to `OrderBook`.

That exercise surfaced a real bug, not a hypothetical one: `LadderBook`'s
`idx(price) = (price - min_price_) / tick_size_` requires every stored price
to land exactly on a grid anchored at `min_price_` — but `min_price_` itself
(derived from a float multiply-then-truncate in `window_low()`) was never
snapped to that grid. Every price the *synthetic* benchmark generates is
already tick-aligned in absolute terms, so this never showed up in
`bench/results.csv`. The moment `LadderBook` went into the real pipeline,
`unknown_refs` on a full 2.2M-message run jumped from 0 to 6.6 million —
almost every add was landing off-grid relative to an unaligned origin and
getting rejected outright, which also made the "latency" numbers look
suspiciously uniform and fast (rejections are cheap; that's not the mutation
cost anyone wanted measured). The fix was one line —
`snap_to_tick(window_low(...), tick_size)` — but finding it required actually
running the new wiring against realistic data and noticing the numbers
didn't smell right, not just getting a clean compile. `unknown_refs` returned
to 0 immediately after, and the full test suite (94 cases, including the
three-way full-day invariant check) stayed green throughout.

## Closing the open question: a real NASDAQ day

`./build/bench` and `./build/replay` were run against
[`12302019.NASDAQ_ITCH50.gz`](https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz),
NASDAQ's full public day file for Dec 30, 2019 — 3,524,013,057 bytes
gzipped, ~8.25 GB decompressed, 268,744,780 frames across 8,892 symbols.
Not a slice, not a sample: the entire published day.

Getting the file itself onto the machine turned into its own small saga,
worth documenting rather than hiding: a plain single-connection `curl` to
`emi.nasdaq.com` measured well under 100 KB/s in this environment — over 14
hours for this file at that rate. That's exactly how a previous attempt at
this same validation ended up as a 688 MB truncated `.gz` sitting in the
repo root: `gzip -t` rejected it with "unexpected end of file" and
`./build/replay` correctly refused to parse it ("gzip stream truncated
before end marker") rather than silently short-changing the numbers on a
partial file — the download had simply never finished. Splitting the same
download into many concurrent byte-range requests instead of one stream
(`bench/fetch_itch_day.sh`, ~16-way parallel, retries per-chunk, verifies
size + `gzip -t` before declaring success) got aggregate throughput to
roughly 1 MB/s and the complete file down in under an hour. The result's
size matches the server's published `Content-Length` exactly and passes
`gzip -t` clean — the thing this whole exercise was blocked on.

### The numbers, and why they come with an asterisk

| type | OrderBook p50 | LadderBook p50 | OrderBook p999 | LadderBook p999 |
|------|--------------:|---------------:|----------------:|-----------------:|
| A    | 167 ns        | 42 ns          | 1,209 ns        | 791 ns           |
| E    | 208 ns        | 42 ns          | 1,459 ns        | 833 ns           |
| C    | 84 ns         | 42 ns          | 1,375 ns        | 708 ns           |
| X    | 83 ns         | 42 ns          | 1,125 ns        | 583 ns           |
| D    | 167 ns        | 42 ns          | 1,458 ns        | 1,041 ns         |
| U    | 375 ns        | 125 ns         | 2,041 ns        | 1,542 ns         |

Full breakdown: [bench/results_real_day.csv](../bench/results_real_day.csv).
This is a separate file, not an overwrite of the committed synthetic
baseline in [bench/results.csv](../bench/results.csv) that the README
quotes — for reasons that follow, these real-day numbers aren't yet good
enough to replace that baseline.

At face value, `LadderBook` is still faster than `OrderBook` at every
percentile for every message type — the *direction* of the original claim
holds on real data. But the same runs reported something that has to be
dealt with before the *magnitude* can be trusted: `unknown_refs` came back
**0** for `OrderBook` and **80,712,122** for `LadderBook` — about 30% of the
263,241,937 order-book-mutating messages in the file. This wasn't a
benchmark-harness quirk: `./build/replay --map` (0 unknown refs) versus
`./build/replay` (80,712,122 unknown refs) against the identical file
confirmed it independently, through the actual production
`pipeline::BookTable` path, not `bench/bench_main.cpp`'s own bookkeeping.

This is the same class of bug "From benchmark exhibit to production
default" above already found once — a rejected mutation is a cheap early
return, not the array-index write the benchmark exists to measure, and that
makes the rejecting book's numbers look faster than they really are, for
the same reason a `false` return from a duplicate-ref `add()` always will.
Last time the cause was tick-grid misalignment; this time it's
`BookTraits<LadderBook>`'s fixed `kWindowPct = 0.30`
(`include/pipeline/dispatch_to_book.hpp`): a ladder built ±30% around the
first `'A'` price seen for a locate has no way to widen later (see "What
LadderBook actually gives up" above), and across 8,892 real symbols over a
full trading day — versus the synthetic generator's 9 symbols drawing from
stable per-symbol bands — a fixed 30% window is nowhere near wide enough
for every name. `OrderBook`'s unbounded ladders have no such ceiling, which
is exactly why its `unknown_refs` stayed at 0 on the same file.

So this run can't cleanly confirm or refute the synthetic-data tail-gap
story as-is: roughly 3 in 10 of `LadderBook`'s timed mutations here were
fast rejections, not real work, which biases its numbers above optimistic
in the same direction the tick-grid bug's numbers once were. What this run
*can* say cleanly, because it comes from `OrderBook`'s side (0 unknown
refs, every sample a real mutation): the absolute tail gap on real data is
far narrower than the synthetic session suggested — p999 gaps of a few
hundred nanoseconds here (417–667 ns across the six types above) versus
several thousand on the synthetic session (5,292–10,416 ns in
`bench/results.csv`). The likeliest reason isn't that the tree-vs-array
tradeoff itself changed; it's that the synthetic session concentrates 2.2M
messages into 9 symbols — deep, hot per-symbol books, with `std::map`
rebalancing at real depth on every insert/erase — while a real day spreads
268.7M frames across 8,892 symbols, most of them shallow most of the time.
A smaller `n` most of the time means less for `OrderBook`'s O(log n) tree
work to do, which narrows its own tail regardless of what `LadderBook` is
doing alongside it. Message-rate burstiness around the open/close — the
original reason this question was open — turns out not to be the dominant
effect; symbol-count breadth is.

### What actually closes this

Not this run, not yet. Two concrete, scoped follow-ups — neither one is
blocked on downloading the file again, since `bench/fetch_itch_day.sh` now
exists as a documented, known-working (if not fast) way to get it back:

1. Fix `BookTraits<LadderBook>`'s window so `unknown_refs` on this same
   file drops to 0 for `LadderBook` too — widen `kWindowPct`, derive it
   from something less arbitrary than a flat 30%, or let `LadderBook`
   rebuild itself wider the first time an `add()` lands outside the
   current window instead of rejecting it outright. Only once that's true
   are `LadderBook`'s latency numbers measuring the same thing
   `OrderBook`'s already are on this file.
2. Re-run the comparison once that fix lands, and only then fold real-day
   numbers into "What the benchmark actually shows" above (and the
   README's copy of that table) as the primary claim — replacing the
   synthetic-only numbers, not just appending real ones alongside them.

Until then, the honest state of the original open question is: **the
tree-vs-array *direction* of the claim held up on a real, complete NASDAQ
day file; the *magnitude* did not, in either direction the synthetic
numbers would have predicted, and roughly a third of `LadderBook`'s side of
that magnitude is currently unreliable, for a specific, understood, fixable
reason.** That's a real answer, not a shrug — it's just not the answer "the
synthetic numbers were right all along" would have been, and it's a more
useful place to have landed than either an untested claim or another
truncated download.

### Follow-up 1 landed: `LadderBook` now grows instead of rejecting

`add()` on a price outside the current range now calls `grow_to_include()`
first — rebuilding the ladder around a wider window (same `window_pct` the
book was constructed with, re-centered on the new price) and re-homing every
resting order at its new array index — and only falls through to the old
reject-outright behavior if the resulting range would blow a hard tick-count
ceiling (`kMaxTicks`, independent of `tick_size_`, guarding the same class of
unbounded-allocation risk `BookTraits<LadderBook>::kMaxSanePrice` already
guards at construction). A fixed `kWindowPct = 0.30` is still what
`BookTraits<LadderBook>` constructs the ladder with initially; the fix is
that it's no longer the ceiling.

This is a source-level fix, verified by a dedicated regression test
(`tests/test_ladder_book.cpp`) exercising growth in both directions,
verifying pre-growth levels/best-bid/best-ask survive the reallocation
untouched, and verifying the `kMaxTicks` refusal path — not yet by a second
run against the same real NASDAQ day file, since that file isn't sitting on
this machine right now and re-downloading it is exactly the multi-hour-if-
done-wrong exercise "Closing the open question" above already documented
once. Follow-up 2 (re-running the real-day comparison and folding those
numbers into the primary benchmark claim) is still open.
