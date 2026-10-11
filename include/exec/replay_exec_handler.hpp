#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <unordered_map>

#include "exec/fill_sim.hpp"
#include "exec/risk_gate.hpp"
#include "exec/types.hpp"
#include "itch/messages.hpp"
#include "pipeline/dispatch_to_book.hpp"

// The exec-layer glue described in docs/architecture.md's gap: turns book
// mutations for one traded stock-locate into exec::Bbo/exec::TradeTick
// events, feeds a live ExecutionStrategy, drains its ChildOrderQueue through
// exec::apply (RiskGate) and FillSimulator, in tape order. Header-only (not
// folded into src/replay_exec_main.cpp) for the same reason the rest of
// exec/ is: so it's directly test-exercised (see tests/test_replay_exec.cpp)
// rather than only reachable by running the compiled binary.
namespace exec {

// Neither book::OrderBook nor book::LadderBook exposes a per-ref lookup —
// each only ever needs one internally, for its own mutators (E/C/X/D/U),
// never for a caller to read back. 'E'/'C' carry only ref+shares, so
// reconstructing a TradeTick's side (and, for 'E', its price) needs that
// read-back — this is new bookkeeping at the wrapper level, scoped to just
// the one locate a run trades, not a change to either book.
struct RestingOrderInfo {
    itch::Side side;
    std::uint32_t price;
};

// Twap and AlmgrenChriss are clock-driven (advance(Timestamp)); Vwap and Pov
// are tape-driven (on_trade_tick_impl only — advance() doesn't exist on
// them). Detecting advance() at compile time lets ReplayExecHandler drive
// all four through one template instead of forking the dispatch loop per
// strategy.
template <typename S, typename = void>
struct has_advance : std::false_type {};
template <typename S>
struct has_advance<S, std::void_t<decltype(std::declval<S&>().advance(exec::Timestamp{}))>>
    : std::true_type {};

inline const char* reject_reason_name(RejectReason r) {
    switch (r) {
        case RejectReason::None: return "none";
        case RejectReason::OrderTooLarge: return "order_too_large";
        case RejectReason::NotionalTooLarge: return "notional_too_large";
        case RejectReason::PriceOutOfCollar: return "price_out_of_collar";
        case RejectReason::CumulativeSharesExceeded: return "cumulative_shares_exceeded";
        case RejectReason::CumulativeNotionalExceeded: return "cumulative_notional_exceeded";
        case RejectReason::KillSwitchTripped: return "kill_switch_tripped";
    }
    return "?";
}

// Wraps a pipeline::BookBuilder<BookType> (same book-building every other
// replay_* binary uses) plus the exec-layer glue above. On every mutation to
// the traded locate, publishes a fresh Bbo to the strategy and
// FillSimulator; on every 'E'/'C' for that locate, reconstructs and
// publishes a TradeTick; drains whatever the strategy pushed into its
// ChildOrderQueue through exec::apply(gate, ...) and FillSimulator::fill.
// Same no-lookahead discipline fill_sim.hpp documents: a fill only ever sees
// book/tape state as of the event that produced its ChildOrder, never
// anything later.
//
// Matches the exact itch::dispatch Handler interface (on_add/on_execute/
// .../on_other) that itch::parse_stream and io::GzipSource::run both expect
// — same convention as pipeline::BookBuilder and replay_query_main.cpp's
// PublishingHandler.
template <typename BookType, typename Strategy>
struct ReplayExecHandler {
    // A one-line constructor rather than an aggregate + designated-init at
    // call sites: BookBuilder::books is a BookTable<BookType>, whose
    // constructor is explicit (see pipeline/book_table.hpp), and an
    // aggregate's omitted member is initialized "as if by empty
    // initializer list" — copy-list-init, which cannot call an explicit
    // constructor, no matter how `inner`'s own default member initializer
    // is spelled. Giving this a constructor makes it a non-aggregate, so
    // `inner` is plain default-initialized (like every other replay_*
    // binary's `BookBuilder<BookType> h;`) instead of copy-list-initialized.
    ReplayExecHandler(std::uint16_t locate_, Strategy& strategy_, RiskGate& gate_,
                      FillSimulator& sim_)
        : locate(locate_), strategy(strategy_), gate(gate_), sim(sim_) {}

    pipeline::BookBuilder<BookType> inner;
    std::uint16_t locate;
    Strategy& strategy;
    RiskGate& gate;
    FillSimulator& sim;
    std::unordered_map<std::uint64_t, RestingOrderInfo> resting{};
    ChildOrderQueue accepted{};
    std::size_t total_accepted = 0;
    std::size_t total_attempted = 0;
    std::array<RejectReason, kMaxChildOrders> reasons{};
    std::array<std::uint64_t, 8> reject_counts{};

    void tick(Timestamp ts) {
        if constexpr (has_advance<Strategy>::value) {
            if (strategy.advance(ts) > 0) drain();
        }
    }

    void publish_bbo(Timestamp ts) {
        const BookType* book = inner.books.find(locate);
        if (book == nullptr) return;

        Bbo bbo;
        bbo.ts = ts;
        if (const auto q = book->best_bid()) {
            bbo.bid_price = q->price;
            bbo.bid_shares = static_cast<Shares>(q->shares);
        }
        if (const auto q = book->best_ask()) {
            bbo.ask_price = q->price;
            bbo.ask_shares = static_cast<Shares>(q->shares);
        }
        // Collar against whichever side of the touch is set — the same
        // "0 == not set" sentinel Bbo/RiskGate already use (see
        // risk_gate.hpp), so an as-yet-one-sided book still gets a
        // reference price instead of leaving the collar unset.
        gate.set_reference_price(bbo.bid_price != 0 ? bbo.bid_price : bbo.ask_price);

        strategy.on_bbo_change(bbo);
        sim.on_bbo_change(bbo);
        drain();
    }

    void maybe_trade_tick(const itch::Header& hdr, std::uint64_t ref, std::uint32_t shares,
                          std::uint32_t price_override) {
        if (hdr.locate != locate) return;
        const auto it = resting.find(ref);
        if (it == resting.end()) return;  // unknown ref (feed gap) — nothing to attribute this to

        const TradeTick tick_event{hdr.timestamp,
                                   price_override != 0 ? price_override : it->second.price, shares,
                                   it->second.side};
        strategy.on_trade_tick(tick_event);
        sim.on_trade_tick(tick_event);
        drain();
    }

    void drain() {
        total_attempted += strategy.child_orders().size();
        const std::size_t n = apply(gate, strategy.child_orders(), accepted, &reasons);
        for (std::size_t i = 0; i < strategy.child_orders().size(); ++i)
            ++reject_counts[static_cast<std::size_t>(reasons[i])];
        for (const auto& order : accepted) sim.fill(order);
        total_accepted += n;
        strategy.child_orders().clear();
        accepted.clear();
    }

    void on_add(const itch::AddOrder& m) {
        tick(m.hdr.timestamp);
        inner.on_add(m);
        if (m.hdr.locate == locate) resting[m.ref] = RestingOrderInfo{m.side, m.price};
        publish_bbo(m.hdr.timestamp);
    }
    void on_execute(const itch::OrderExecuted& m) {
        tick(m.hdr.timestamp);
        maybe_trade_tick(m.hdr, m.ref, m.shares, 0);
        inner.on_execute(m);
        publish_bbo(m.hdr.timestamp);
    }
    void on_execute_price(const itch::OrderExecutedPrice& m) {
        tick(m.hdr.timestamp);
        maybe_trade_tick(m.hdr, m.ref, m.shares, m.price);
        inner.on_execute_price(m);
        publish_bbo(m.hdr.timestamp);
    }
    void on_cancel(const itch::OrderCancel& m) {
        tick(m.hdr.timestamp);
        inner.on_cancel(m);
        publish_bbo(m.hdr.timestamp);
    }
    void on_delete(const itch::OrderDelete& m) {
        tick(m.hdr.timestamp);
        if (m.hdr.locate == locate) resting.erase(m.ref);
        inner.on_delete(m);
        publish_bbo(m.hdr.timestamp);
    }
    void on_replace(const itch::OrderReplace& m) {
        tick(m.hdr.timestamp);
        if (m.hdr.locate == locate) {
            const auto it = resting.find(m.orig_ref);
            const itch::Side side = it != resting.end() ? it->second.side : itch::Side::Buy;
            resting.erase(m.orig_ref);
            resting[m.new_ref] = RestingOrderInfo{side, m.price};
        }
        inner.on_replace(m);
        publish_bbo(m.hdr.timestamp);
    }
    void on_other(char type, std::size_t len) { inner.on_other(type, len); }
};

}  // namespace exec
