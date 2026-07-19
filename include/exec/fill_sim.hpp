#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "exec/types.hpp"

// Fill simulation for the ChildOrders that Twap/Vwap/Pov push into a
// ChildOrderQueue. Nothing upstream of this file simulates what happens to a
// child order once it's scheduled — this closes that gap so a strategy's
// output can be scored (realized average price, slippage vs. arrival price,
// fill rate) instead of just inspected as a list of intentions.
//
// Scope — this is a lightweight fill model, not a matching engine, and that's
// a deliberate scope decision:
//
//   * A Limit ChildOrder fills IMMEDIATELY AND FULLY if it's marketable
//     against the current opposite-side best quote at the instant it's
//     evaluated (Buy >= best_ask fills at best_ask, Sell <= best_bid fills at
//     best_bid). If it isn't marketable it simply doesn't fill — no resting,
//     no partial fills, no revisiting it on a later quote. That means this
//     model UNDERESTIMATES fill rate for passive limit orders (a real one
//     would sit on the book and could fill later as the market moves to it);
//     teaching FillSimulator to track resting orders against a replayed book
//     is the main thing a v2 would add.
//   * A Market ChildOrder (Pov's output) fills immediately at the price of
//     the TradeTick that triggered it. That's defensible, not just
//     convenient: Pov only ever pushes a child off an actual observed print,
//     so that print's price already IS the fill price, with zero assumed
//     slippage — there is no better price to assign it.
//
// Same no-lookahead discipline as the rest of this codebase applies here:
// a fill can only use book/tape state as of the ChildOrder's own timestamp,
// never anything later. This is enforced by construction, not by checking a
// timestamp: FillSimulator only ever sees the rolling Bbo/last-trade-price as
// of "now" because the caller is expected to feed on_bbo_change/on_trade_tick
// in tape order, then evaluate fill() for any child order generated off that
// same event, before advancing to the next one.
namespace exec {

// One realized execution. Field order matches how a caller will typically
// want to log or print it: when, which side, at what price, how much.
struct Fill {
    Timestamp ts = 0;
    itch::Side side = itch::Side::Buy;
    Price price = 0;
    Shares shares = 0;
};

// A ChildOrder either fills in full right away or not at all under this
// model (see scope note above), so at most one Fill exists per ChildOrder —
// the same capacity as ChildOrderQueue covers every possible fill.
inline constexpr std::size_t kMaxFills = kMaxChildOrders;

// Fixed-capacity, allocation-free sink for fills — same std::array + size
// pattern as ChildOrderQueue in exec/types.hpp.
class FillQueue {
public:
    bool push(const Fill& f) {
        if (size_ >= kMaxFills) return false;
        buf_[size_++] = f;
        return true;
    }
    void clear() { size_ = 0; }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    bool full() const { return size_ >= kMaxFills; }

    const Fill* begin() const { return buf_.data(); }
    const Fill* end() const { return buf_.data() + size_; }

private:
    std::array<Fill, kMaxFills> buf_{};
    std::size_t size_ = 0;
};

// Replays ChildOrders against a rolling quote/tape state and accumulates
// fill stats. Deliberately decoupled from book::OrderBook: this class never
// sees a book, only the Bbo/TradeTick snapshots the caller feeds it (from
// OrderBook::best_bid()/best_ask() after each mutation, and from the tape
// directly), so it can be exercised in isolation with hand-built scenarios —
// exactly what tests/test_fill_sim.cpp does.
class FillSimulator {
public:
    // Caller feeds these in tape order, same convention as
    // exec::ExecutionStrategy's on_bbo_change/on_trade_tick: update the
    // rolling state first, then evaluate fill() for whatever ChildOrders
    // that event produced. A Market ChildOrder carries no price of its own
    // (see exec::ChildOrder — price is defined as ignored for
    // OrderType::Market), so on_trade_tick's job is exactly to supply the
    // price that fill() uses for it: the triggering print's price, per the
    // scope note above.
    void on_bbo_change(const Bbo& bbo) { bbo_ = bbo; }
    void on_trade_tick(const TradeTick& tick) { last_trade_price_ = tick.price; }

    const Bbo& bbo() const { return bbo_; }
    Price last_trade_price() const { return last_trade_price_; }

    // Evaluates one ChildOrder against the current rolling state and records
    // a Fill if it's immediately marketable. Returns whether it filled.
    //
    // order.ts already carries the instant this order was generated (the bin
    // boundary for Twap, the triggering tick's timestamp for Vwap/Pov) — that
    // is the trigger timestamp this call needs, so there's no separate
    // trigger_ts parameter here; folding it into ChildOrder rather than
    // threading it through a second argument avoids two timestamps that must
    // always agree.
    bool fill(const ChildOrder& order) {
        shares_attempted_ += order.shares;

        Price fill_price = 0;
        bool marketable = false;

        if (order.type == OrderType::Market) {
            // Zero is the "no print seen yet" sentinel (see Bbo's own
            // price == 0 convention in types.hpp) — a market child pushed
            // before any on_trade_tick is a caller sequencing bug, not
            // something to fill at a fabricated price.
            fill_price = last_trade_price_;
            marketable = (fill_price != 0);
        } else if (order.side == itch::Side::Buy) {
            if (bbo_.ask_price != 0 && order.price >= bbo_.ask_price) {
                fill_price = bbo_.ask_price;
                marketable = true;
            }
        } else {
            if (bbo_.bid_price != 0 && order.price <= bbo_.bid_price) {
                fill_price = bbo_.bid_price;
                marketable = true;
            }
        }

        if (!marketable) return false;
        if (!fills_.push(Fill{order.ts, order.side, fill_price, order.shares})) return false;

        shares_filled_ += order.shares;
        // Widen to unsigned __int128 before the multiply, same
        // overflow-safety pattern as exec::Vwap/exec::Pov: price and shares
        // are each uint32_t, so their product alone can already exceed
        // uint64_t headroom once summed across a full day's fills.
        notional_ += static_cast<unsigned __int128>(fill_price) *
                     static_cast<unsigned __int128>(order.shares);
        return true;
    }

    const FillQueue& fills() const { return fills_; }

    Shares shares_filled() const { return shares_filled_; }
    Shares shares_attempted() const { return shares_attempted_; }

    // shares filled / shares attempted, as a display-only ratio — 0 if
    // nothing has been attempted yet rather than dividing by zero.
    double fill_rate() const {
        if (shares_attempted_ == 0) return 0.0;
        return static_cast<double>(shares_filled_) / static_cast<double>(shares_attempted_);
    }

    // Volume-weighted average fill price. notional_ is the integer
    // accumulation (sum of price*shares); this only converts to floating
    // point here, as a final display value, not on any hot path.
    double vwap_price() const {
        if (shares_filled_ == 0) return 0.0;
        return static_cast<double>(notional_) / static_cast<double>(shares_filled_);
    }

private:
    Bbo bbo_{};
    Price last_trade_price_ = 0;

    FillQueue fills_;
    Shares shares_filled_ = 0;
    Shares shares_attempted_ = 0;
    unsigned __int128 notional_ = 0;  // sum(price * shares) over every fill
};

}  // namespace exec
