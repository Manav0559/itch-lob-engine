# Fuzzing the ITCH parser

`itch::parse_stream` / `itch::dispatch` (`include/itch/parser.hpp`) is the one
place in this engine where untrusted bytes — a file on disk, or in principle a
live multicast feed — get interpreted as structured data. The unit tests in
`tests/test_parser.cpp` cover known-shape corruption (truncated tails, unknown
types, mismatched frame lengths) by construction. This directory adds
coverage-guided fuzzing on top of that: genuinely adversarial byte sequences,
generated and mutated by libFuzzer under ASan + UBSan.

## What's covered

`fuzz_parser.cpp` does not stop at decode. It feeds fuzzer input through
`itch::parse_stream` into a real `pipeline::BookBuilder` — the same struct
`replay_main.cpp` and `replay_threaded_main.cpp` both drive — which applies
every decoded message to a real `book::OrderBook`, one per stock locate.
Fuzzing decode alone would miss bugs that only surface once a
malformed-but-decodable message sequence reaches the book: an order-id
collision, a replace/cancel/execute referencing a ref that was never added or
was already retired, a delete racing a replace. `BookBuilder` is reused
as-is (`include/pipeline/dispatch_to_book.hpp`), not reimplemented, so this
harness never drifts from what the real replay binaries do with the same
bytes.

## Running it

```sh
fuzz/build_and_run.sh            # 60s smoke run (default)
fuzz/build_and_run.sh 300        # 300s
FUZZ_SECONDS=300 fuzz/build_and_run.sh   # same, via env var
```

Requires `clang++` with the libFuzzer runtime. On macOS, the Xcode Command
Line Tools ship ASan/UBSan but *not* `libclang_rt.fuzzer_osx.a` — install
LLVM via Homebrew (`brew install llvm`) and make sure that `clang++` (e.g.
`$(brew --prefix llvm)/bin/clang++`) resolves ahead of the CLT one on `PATH`,
or point `CXX`/adjust the script's `clang++` call at it directly. On Linux,
a stock `clang++` from LLVM apt/dnf packages already includes it.

The script compiles `fuzz_parser.cpp` with:

```
clang++ -std=c++20 -Iinclude -fsanitize=fuzzer,address,undefined -g -O1
```

then runs the binary seeded from `fuzz/corpus/` and `fuzz/corpus/regressions/`
for the given duration. New coverage-increasing inputs libFuzzer discovers
along the way are written to `fuzz/findings/` (gitignored, created on first
run) rather than back into `fuzz/corpus/` itself — a short run turns up
hundreds of mutations, and committing those as if they were curated seeds
would be noise, not signal. `fuzz/corpus/` only grows when a run is promoted
into it deliberately (see `regressions/` below). This is intentionally **not**
wired into `CMakeLists.txt` or
`ctest`: libFuzzer isn't available the same uniform way across this project's
Linux/macOS CI matrix as the normal build, and mixing sanitizer flags into
the main `-Wall -Wextra -Wpedantic -Werror` build would be its own mess. A
standalone script is the right amount of integration for a fuzz smoke test.

## `corpus/`

Seed inputs, generated with the same `itch::encode` mirror encoders
`tests/test_book.cpp`, `tests/test_parser.cpp` and `replay_main.cpp`'s
`selftest()` use (`fuzz/generate_corpus.cpp` — a standalone one-shot tool,
not part of the CMake build; rerun and recompile it by hand if the corpus
ever needs regenerating). Real framed ITCH structure gets the fuzzer past the
2-byte length-prefix gate immediately instead of spending most of its budget
on inputs that die at frame parsing:

- `selftest.bin` — byte-for-byte the same session as `replay_main.cpp`'s
  `--selftest`: two symbols, adds through a replace and a delete, plus one
  undecoded type (`'S'`).
- `mixed_types.bin` — every decoded message type including `'F'` (attributed
  add) and `'C'` (executed at a different price) across two locates, plus one
  undecoded type (`'H'`, trading halt).
- `replace_chain.bin` — a chain of replaces on one order lineage, followed by
  deletes/cancels against refs already retired by an earlier replace in the
  same stream (the shape a real feed gap produces).
- `boundary_values.bin` — locate 0 and `0xFFFF`, a zero-share add, price 0
  and `UINT32_MAX`, and a duplicate order ref reused on the opposite side.

## `corpus/regressions/`

Every crash libFuzzer finds and that gets fixed is archived here permanently
(exact crashing input, trimmed if libFuzzer's own minimizer shrank it) and
replayed on every future run, in addition to being turned into a normal
Catch2 test in `tests/test_parser.cpp` or `tests/test_book.cpp`. The two
aren't redundant: the Catch2 test runs on every `ctest` invocation without
anyone needing clang or libFuzzer installed; the corpus entry keeps that same
input in the fuzzer's own seed pool so future mutations continue to explore
around it.

## Outcome of the last run

Clean. Two runs against the 4-entry seed corpus above (`corpus/regressions/`
still empty — nothing has needed to be archived there), ~4.6M total
executions across both, no crash, no ASan/UBSan report:

- 90s run: 1,713,866 executions, edge coverage climbed from 651 to 1,214.
- 60s run (after reshuffling the script to keep findings out of the tracked
  corpus, see above): 2,872,686 executions, same result.

Both runs grew a local `fuzz/findings/` corpus (gitignored, not part of this
commit) of a few hundred coverage-increasing mutations, all of which decoded
and applied to the book without incident — nothing to report or fix this
round. `parse_stream`'s length-prefix framing and `dispatch`'s expect-length
check appear to do their job: the only way to reach the decoded field paths
is with a frame the wire format itself makes structurally valid, so most
mutation effort quickly converges on shapes the parser already handles by
rejecting them via `on_other`.

This is a clean-bill for the coverage this harness reaches, not a claim that
no bug exists anywhere in the parser+book path — a longer run (`fuzz/build_and_run.sh 3600`
or more, ideally on Linux with libFuzzer's fork-server mode for higher
throughput) is worth doing periodically as the corpus and code evolve.

## Differential fuzzing: OrderBook vs. LadderBook (`fuzz_differential.cpp`)

`fuzz_parser.cpp` above fuzzes decode-into-one-book. It structurally cannot
see a whole class of bug: `book::OrderBook` (the `std::map`-ladder v1
baseline) and `book::LadderBook` (the flat tick-ladder that's the production
default — see [docs/devlog-orderbook-vs-ladderbook.md](../docs/devlog-orderbook-vs-ladderbook.md))
are supposed to be interchangeable — `replay`/`replay_threaded`'s `--map`
flag picks either one for byte-identical input — but nothing previously
checked that they actually agree.

`fuzz_differential.cpp` does: it replays the same decoded message stream
into a `pipeline::BookBuilder<book::OrderBook>` and a
`pipeline::BookBuilder<book::LadderBook>` side by side (both reused as-is
via their real `on_add`/`on_execute`/... entry points, never reimplemented),
and after every message both books actually applied, diffs their full public
contract: `best_bid()`, `best_ask()`, `open_orders()`, `bid_levels()`,
`ask_levels()`. Any disagreement is a bug by construction — no hand-written
oracle, no fixture to keep in sync by hand — and calls `std::abort()` so
libFuzzer records it as a crash with a minimized reproducer.

### The policy-divergence guard

The two books do **not** have identical acceptance policies, on purpose:
`LadderBook`'s tick-grid rejects a price outside its (growable, but
hard-capped at `kMaxTicks`) window or off the tick grid; `OrderBook`'s
`std::map` ladders accept any price. This is documented, intentional
behavior — the devlog above measured this exact policy once silently
dropping ~30% of a real NASDAQ day's mutations under a fixed window, which
is why `LadderBook`'s window can now grow at all. A naive differential
harness would flag every one of those as a "crash" the instant a
libFuzzer-generated price landed outside the window, which is on
essentially the *first* mutated message of almost *every* run — pure
false-positive noise, not a finding.

`fuzz_differential.cpp` tells the two apart by comparing `unknown_refs`
deltas (`BookBuilder` already bumps this on any add/replace/execute/cancel/
delete its own book rejects) before and after applying a message to each
book:

- Both books applied it, or both rejected it → no real disagreement; diff
  state only when both applied it.
- One applied it and the other didn't → an acceptance-**policy** divergence,
  not a state bug. That locate is marked and never compared again for the
  rest of that input — once the books disagree on which orders even exist
  for a symbol, further state comparison is meaningless, not just noisy.

### Running it

```sh
fuzz/build_and_run_differential.sh            # 60s smoke run (default)
fuzz/build_and_run_differential.sh 300        # 300s
FUZZ_SECONDS=300 fuzz/build_and_run_differential.sh
```

Same toolchain requirement and macOS Homebrew-LLVM-`clang++` fallback as
`fuzz/build_and_run.sh` above. Findings land in `fuzz/findings/differential/`
(gitignored) rather than `fuzz/findings/`, so a differential-fuzzing session
never mixes its corpus growth into the parser harness's pool; both harnesses
still read the same `fuzz/corpus/` and `fuzz/corpus/regressions/` seeds,
since both decode the same wire format.

### A measured cost worth knowing about, not a bug

This harness runs meaningfully slower than `fuzz_parser.cpp` — tens of
executions/second, not millions. That's real, and it's `LadderBook`'s own
construction cost, not fuzzer overhead: every new locate a run touches
allocates two flat `Level` vectors sized to that locate's price window (up
to `kMaxTicks` per side at the extreme), where `OrderBook`'s `std::map`
ladders only ever allocate per occupied level. Feeding real wire-derived,
unconstrained prices into `LadderBook` — the entire point of this harness —
means periodically paying that allocation cost once per newly-seen locate,
which dominates wall-clock time far more than ASan/UBSan's usual overhead
does. Shrinking the harness's own input space to dodge this would also dodge
the exact adversarial-price territory the harness exists to exercise, so
this is accepted as a real, measured property of testing `LadderBook` this
way — not something to engineer around.

### Outcome of the last run

Clean. 90s against the same 4-entry seed corpus `fuzz_parser.cpp` uses:

- 4,211 total executions (`~46/s` — see "A measured cost" above for why
  this is throughput-bound, not a fluke).
- 4,759 message applications where both books agreed and were compared —
  all agreed, zero state divergences, zero `std::abort()`s.
- 2,520 policy-divergence skips correctly identified and set aside as the
  documented `LadderBook` price-window behavior, not book bugs.

As with `fuzz_parser.cpp`, this is a clean bill for the coverage this run
reached, not a claim the books can never disagree — the same "run it longer
periodically" recommendation applies, and a natural next step is a custom
libFuzzer mutator that operates on whole decoded ITCH frames (duplicate a
frame, splice a cancel/delete after the add that introduced its ref, nudge a
price off-tick) instead of raw bytes, so mutation pressure lands on exactly
the shapes likely to stress the two books differently instead of mostly
dying at the 2-byte length-prefix gate.
