# Architecture

One page, meant to be read in under 10 minutes: what this engine does, the
two ways data enters it, and where each of the ~15 files under `include/`
sits in the flow. The README's Status list documents *what's been built and
measured*; this doc is the map you'd want before reading any of it — start
here, then go to the README for the numbers and `docs/devlog-orderbook-vs-
ladderbook.md` for the one big design tradeoff written up in full.

## What this is, in one paragraph

A NASDAQ TotalView-ITCH 5.0 feed handler: it decodes the exchange's real
binary wire protocol into typed messages (`itch::parse_stream` /
`itch::dispatch`), replays them into a live limit order book per stock
symbol (`BookBuilder` routing into `OrderBook` or `LadderBook`, one per
`stock locate`), and can schedule and risk-check child orders
(Twap/Vwap/Pov/Almgren-Chriss → `RiskGate` → `FillSimulator`) directly off
that live book's quotes and prints (`./build/replay_exec`). Two independent
input paths (file replay and live UDP multicast) feed the same
book-building core; two book implementations (`std::map`-based and
flat-array-based) share the identical interface behind that core, swappable
with one command-line flag.

## Data flow

```mermaid
flowchart TD
    subgraph Input["Input paths"]
        F1["day file (.NASDAQ_ITCH50 or .gz)\nio::MmapSource / io::GzipSource"]
        F2["UDP multicast feed\n(NASDAQ MoldUDP64 session)\nnet::MoldUdp64Receiver"]
    end

    subgraph Decode["Decode: raw bytes -> typed message"]
        P1["itch::parse_stream / itch::dispatch\n(include/itch/parser.hpp)\nlength-prefixed framing, big-endian wire ints"]
    end

    subgraph Threading["Optional thread hop"]
        Q["QueueProducer -> SpscQueue<Envelope> -> consume()\n(include/pipeline/threaded_replay.hpp, replay_threaded only)\nparser thread and book-builder thread, decoupled"]
    end

    subgraph BookCore["Shared book-building core"]
        BB["pipeline::BookBuilder\n(include/pipeline/dispatch_to_book.hpp)\non_add / on_execute / on_cancel / on_delete / on_replace"]
        BT["pipeline::BookTable<BookType>\n(include/pipeline/book_table.hpp)\none book per stock locate, dense array-indexed"]
        OB["book::OrderBook\nstd::map ladders (--map flag)"]
        LB["book::LadderBook\nflat tick-indexed vectors (default)"]
    end

    subgraph ExecLayer["Execution layer (replay_exec: wired end to end off a live book)"]
        BBO["exec::Bbo / exec::TradeTick\n(best bid/ask, trade prints — derived from the book)"]
        STRAT["Twap / Vwap / Pov / AlmgrenChriss\n(include/exec/*.hpp)\nExecutionStrategy CRTP -> ChildOrderQueue"]
        RG["exec::RiskGate\n(include/exec/risk_gate.hpp)\nper-order + cumulative limits, kill switch"]
        FS["exec::FillSimulator\n(include/exec/fill_sim.hpp)\nscores ChildOrders against replayed quotes/tape"]
    end

    F1 --> P1
    F2 --> P1
    P1 -- "replay (single-threaded)" --> BB
    P1 -- "replay_threaded" --> Q
    P1 -- "replay_exec" --> BB
    Q --> BB
    F2 -. "live_replay: its own local\nunordered_map<locate,OrderBook>\nhandler, not pipeline::BookBuilder" .-> LiveBB["live_replay's BookBuilder"]
    BB --> BT
    BT --> OB
    BT --> LB
    OB -- "book state (bbo, trade prints)\nexec::ReplayExecHandler\n(include/exec/replay_exec_handler.hpp)" --> BBO
    LB -- "book state (bbo, trade prints)\nexec::ReplayExecHandler\n(include/exec/replay_exec_handler.hpp)" --> BBO
    BBO --> STRAT
    STRAT -- "ChildOrder" --> RG
    RG -- "accepted ChildOrder" --> FS
```

If your renderer doesn't do Mermaid, here's the same shape as plain text:

```
 day file (.gz / mmap)                 UDP multicast (MoldUDP64)
        |                                        |
        v                                        v
 itch::parse_stream/dispatch              net::MoldUdp64Receiver
 (include/itch/parser.hpp)                 -> itch::dispatch
        |                                        |
        | replay: same thread                    | live_replay: its own local
        | replay_threaded: SPSC queue             | BookBuilder (unordered_map
        | to a 2nd thread                         | <locate, OrderBook>) --
        v                                         | see "Live path" below
 pipeline::BookBuilder::on_add/on_execute/on_cancel/on_delete/on_replace
        |
        v
 pipeline::BookTable<BookType>   (one book per stock locate, array-indexed)
        |
        +--> book::OrderBook   (std::map ladders,   --map flag, the A/B baseline)
        +--> book::LadderBook  (flat tick vectors,   default, production choice)
        |
        v  exec::ReplayExecHandler (include/exec/replay_exec_handler.hpp)
        |  wraps BookBuilder for replay_exec; every mutation of the traded
        |  locate publishes a fresh Bbo, every execute a TradeTick
        v
 exec::Bbo, exec::TradeTick
        |
        v
 Twap / Vwap / Pov / AlmgrenChriss  (ExecutionStrategy -> ChildOrderQueue)
        |
        v
 exec::RiskGate   (per-order + cumulative limits, latching kill switch)
        |
        v
 exec::FillSimulator   (scores ChildOrders against the replayed quote/tape)
```

## Entry points, and which parts of the diagram each one exercises

| Binary | Source | Path through the diagram |
|---|---|---|
| `./build/replay` | `src/replay_main.cpp` | file (mmap or gzip-stream) → `itch::parse_stream` → `pipeline::BookBuilder` → `pipeline::BookTable<BookType>` directly, one thread. `BookType` defaults to `LadderBook`; `--map` swaps in `OrderBook`. |
| `./build/replay_threaded` | `src/replay_threaded_main.cpp`, `include/pipeline/threaded_replay.hpp` | same file input and same `BookBuilder`/`BookTable` core as `replay`, but the parser and the book-builder run on separate threads joined by a lock-free `SpscQueue<Envelope>` (`include/pipeline/spsc_queue.hpp`); `dispatch_to_book` re-applies an `Envelope` decoded on the parser thread into the book on the consumer thread. Produces identical book state to `replay` — see `tests/test_replay_threaded.cpp`. |
| `./build/live_replay` | `src/live_replay_main.cpp`, `include/net/multicast_receiver.hpp` | UDP multicast → `net::MoldUdp64Receiver` (session header, sequence-gap detection, retransmission-request round trip) → `itch::dispatch` → **its own local `BookBuilder`** over `std::unordered_map<locate, book::OrderBook>`. This is the one place in the diagram that does *not* go through `pipeline::BookBuilder`/`BookTable` or `LadderBook` — it predates that shared abstraction and hasn't been migrated onto it yet (a good next-step consolidation, not a hidden bug). |
| `./build/multicast_sender` | `src/multicast_sender_main.cpp` | not a book-building path at all — a `MoldUdp64Sender` that replays a synthetic session onto the multicast group `live_replay` joins, and answers its retransmission requests. Exists to exercise `live_replay` without a real feed. |
| `./build/replay_exec` | `src/replay_exec_main.cpp`, `include/exec/replay_exec_handler.hpp` | same file input and same `BookBuilder`/`BookTable` core as `replay`, wrapped by `exec::ReplayExecHandler`: every mutation of one `--locate` publishes a fresh `exec::Bbo` (and, on an execute, an `exec::TradeTick`) to a live `--strategy twap\|vwap\|pov\|almgren_chriss`, drains its `ChildOrderQueue` through `exec::RiskGate`, and scores fills with `exec::FillSimulator` — the strategy → risk → fill layer described below, running end to end instead of only under its own unit tests. |
| `./build/bench`, `./build/bench_threaded` | `bench/bench_main.cpp`, `bench/bench_threaded_main.cpp` | drive the exact same `BookBuilder`/`BookTable` (and, for the threaded one, `pipeline::run_pipeline`) core as `replay`/`replay_threaded` against a synthetic in-memory session, so the benchmarked code path and the production path are provably the same code, not two implementations that happen to agree. |

## The book swap point

`pipeline::BookTable<BookType>` and `pipeline::BookBuilder<BookType>`
(`include/pipeline/dispatch_to_book.hpp`) are the one place `OrderBook` and
`LadderBook` are interchangeable: both expose the identical
`add/execute/cancel/remove/replace` interface, so `BookTable` — and
everything upstream of it (the parser, the threaded queue, the benchmarks)
— is written once, generic over `BookType`, and never needs to know which
book backend it's holding. `BookTraits<BookType>` is the one customization
hook: `OrderBook` is default-constructible, `LadderBook` needs a price
window sized off the first `A` message's price (see the specialization in
`dispatch_to_book.hpp`), and that's the only difference `BookTable` has to
account for. `LadderBook` is the default in both `replay` and
`replay_threaded`; `--map` is the explicit, always-available A/B switch back
to `OrderBook`. See `docs/devlog-orderbook-vs-ladderbook.md` for why both
exist and what the measured tradeoff actually is.

## Where exec/risk/fill sit, and what "sit" means today

`include/exec/*.hpp` (execution strategies), `risk_gate.hpp`, and
`fill_sim.hpp` are real, tested, header-only modules — Almgren-Chriss,
Twap, Vwap, and Pov all implement the same `ExecutionStrategy` CRTP
interface and push `ChildOrder`s into a fixed-capacity `ChildOrderQueue`;
`RiskGate` is the documented, intended checkpoint between that queue and
"wherever orders go next" (its own header comment names `FillSimulator` in
this codebase, an OMS in a live one); `FillSimulator` scores those orders
against a replayed `Bbo`/`TradeTick` stream. `include/exec/replay_exec_handler.hpp`
is the glue that closes what used to be a standalone-only layer: it wraps a
`pipeline::BookBuilder<BookType>` exactly like `replay_query_main.cpp`'s
`PublishingHandler` does, but instead of publishing a snapshot to a query
server, it turns every mutation of one traded `--locate` into an
`exec::Bbo`, and every execute into an `exec::TradeTick`, feeds a live
strategy, and drains whatever it pushes through `exec::apply(RiskGate, ...)`
and `FillSimulator::fill`. `src/replay_exec_main.cpp` is the CLI around it
(`./build/replay_exec --selftest --strategy vwap`); `tests/test_replay_exec.cpp`
drives the same handler directly, off the same synthetic session
`replay`/`replay_query` self-test against, asserting the schedule-advances-
off-the-tape-clock, trade-tick-attribution, and risk-gate-rejection
invariants end to end. `replay`/`replay_threaded`/`live_replay` themselves
are untouched by any of this — `replay_exec` is a fifth, additive binary,
not a change to the three throughput-benchmarked ones.

## File map

```
include/
  itch/       messages.hpp (wire structs), parser.hpp (dispatch/parse_stream), encode.hpp (test/fuzz mirror encoders)
  io/         mmap_source.hpp, gzip_source.hpp — how a day file's bytes reach the parser
  net/        moldudp64.hpp, multicast_receiver.hpp, multicast_test_sender.hpp — the live UDP path
  book/       order_book.hpp (std::map), ladder_book.hpp (flat array) — identical interface, see architecture note above
  pipeline/   book_table.hpp, dispatch_to_book.hpp, message.hpp, spsc_queue.hpp, threaded_replay.hpp, thread_affinity.hpp
              — the shared book-building core and the threaded variant's plumbing
  exec/       types.hpp, execution_strategy.hpp, twap/vwap/pov/almgren_chriss.hpp, volume_curve.hpp, risk_gate.hpp, fill_sim.hpp
              — strategy -> risk -> fill layer described above
              replay_exec_handler.hpp — the book-to-strategy glue replay_exec_main.cpp drives
src/          replay_main.cpp, replay_threaded_main.cpp, live_replay_main.cpp, multicast_sender_main.cpp,
              replay_query_main.cpp, replay_exec_main.cpp — the six binaries
bench/        bench_main.cpp, bench_threaded_main.cpp, synthetic_session.hpp — reuse the same core against a synthetic session
tests/        one file per component; test_full_day_invariants.cpp cross-checks OrderBook, LadderBook, and an independent
              reference model against each other across a whole synthetic day; test_replay_exec.cpp drives
              replay_exec_handler.hpp end to end off a synthetic session
dashboard/    static HTML viewer for the committed bench/*.csv results (see dashboard/README.md)
```
