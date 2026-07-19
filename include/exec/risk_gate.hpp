#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "exec/types.hpp"

// Sits between a strategy's ChildOrderQueue output and wherever orders
// actually go next (e.g. exec::FillSimulator, or a future live-order
// gateway). Twap/Vwap/Pov/AlmgrenChriss will faithfully emit whatever a bad
// schedule, a bad parameter, or a runaway loop tells them to — nothing
// upstream of this file questions an order's size, price, or pace before it
// goes out. This is that checkpoint.
namespace exec {

// Static per-order and per-session caps. Shares/notional fields use the same
// widths as the rest of exec/ (Shares == std::uint32_t, notional widened to
// std::uint64_t) so a caller builds limits directly from the same integers
// ChildOrder itself is made of.
struct RiskLimits {
    Shares max_order_shares = 0;
    std::uint64_t max_order_notional = 0;  // price * shares for a single order

    // Max allowed deviation of a Limit order's price from the gate's current
    // reference price, in basis points of that reference price. Meaningless
    // for a Market order — it has no price to collar against, per
    // ChildOrder's own "price ignored when type == Market" convention — so
    // RiskGate::check() skips this for Market orders rather than collaring
    // against a fabricated price.
    std::uint32_t price_collar_bps = 0;

    Shares max_cumulative_shares = 0;
    std::uint64_t max_cumulative_notional = 0;
};

// A caller needs to know WHY an order was rejected: a bare boolean gate
// can't distinguish a strategy bug (order too large) from a stale reference
// price (collar) from a runaway session (cumulative / kill switch), and each
// of those calls for a different human response. Declaration order here
// matches the precedence RiskGate::check() evaluates them in.
enum class RejectReason : std::uint8_t {
    None,
    OrderTooLarge,
    NotionalTooLarge,
    PriceOutOfCollar,
    CumulativeSharesExceeded,
    CumulativeNotionalExceeded,
    KillSwitchTripped,
};

// Gate between "the strategy decided to trade" and "the order actually goes
// out".
//
// check() and record() are deliberately two calls, not one: check() alone
// lets a caller ask "would this pass?" without committing it to the running
// cumulative totals — a speculative what-if, or simply not yet knowing
// whether the order will actually make it downstream — and it keeps this
// gate from having to guess whether a passing order will really be sent.
// Only record(), called once the caller has actually forwarded the order,
// advances the cumulative counters. apply() below is the check-then-record
// pairing most callers actually want.
//
// Kill switch: crossing max_cumulative_shares or max_cumulative_notional
// does not just reject the order that crossed it — it LATCHES this gate
// into a tripped state, and every order after that is rejected with
// KillSwitchTripped regardless of its own size, forever, until reset() is
// called. This mirrors how a real risk kill switch behaves: once cumulative
// volume breaches the limit, the working assumption is that something
// upstream is broken, and the fix is a human looking at it — not the gate
// quietly reopening the moment volume happens to dip back under the limit.
class RiskGate {
public:
    explicit RiskGate(const RiskLimits& limits, Price reference_price = 0)
        : limits_(limits), reference_price_(reference_price) {}

    // 0 is the "no reference yet" sentinel — same convention as
    // Bbo::bid_price/ask_price in types.hpp — so the collar check is simply
    // skipped until a caller has fed at least one price through here.
    // Typical callers feed this from the current BBO (e.g. the mid, or
    // whichever side the order trades against) on every on_bbo_change, or
    // once from the parent order's arrival price; which one is a policy
    // choice this gate deliberately leaves to the caller rather than
    // presuming.
    void set_reference_price(Price price) { reference_price_ = price; }

    // Evaluates order against every per-order limit, then the cumulative
    // limits, returning the first violated RejectReason (checked in
    // RejectReason's own declaration order) or None if it passes all of
    // them. Crossing a cumulative limit trips the kill switch as a side
    // effect of this call — that is real gate state, not deferred to
    // record() — but the order's own contribution to the running totals is
    // not added until record() is called; see the class comment for why.
    RejectReason check(const ChildOrder& order) {
        if (tripped_) return RejectReason::KillSwitchTripped;

        if (order.shares > limits_.max_order_shares) return RejectReason::OrderTooLarge;

        // Widen before multiplying — same pattern as exec::Vwap/Pov: Price
        // and Shares are each uint32_t, so their product alone can exceed
        // 32-bit range long before either operand is anywhere near its own
        // max.
        const std::uint64_t order_notional =
            static_cast<std::uint64_t>(order.price) * static_cast<std::uint64_t>(order.shares);
        if (order_notional > limits_.max_order_notional) return RejectReason::NotionalTooLarge;

        if (order.type == OrderType::Limit && reference_price_ != 0) {
            const Price diff = order.price > reference_price_ ? order.price - reference_price_
                                                                : reference_price_ - order.price;
            const std::uint64_t allowed_deviation = static_cast<std::uint64_t>(reference_price_) *
                                                     static_cast<std::uint64_t>(limits_.price_collar_bps) / 10'000;
            if (static_cast<std::uint64_t>(diff) > allowed_deviation) return RejectReason::PriceOutOfCollar;
        }

        if (static_cast<std::uint64_t>(cumulative_shares_) + order.shares > limits_.max_cumulative_shares) {
            tripped_ = true;
            return RejectReason::CumulativeSharesExceeded;
        }
        // unsigned __int128, not uint64_t, for this comparison specifically:
        // cumulative_notional_ and order_notional can each already sit close
        // to 2^64 (a long session near its own limit, hit by one more large
        // order), and their plain sum could wrap a 64-bit accumulator right
        // at the boundary this check exists to catch.
        if (cumulative_notional_ + static_cast<unsigned __int128>(order_notional) >
            static_cast<unsigned __int128>(limits_.max_cumulative_notional)) {
            tripped_ = true;
            return RejectReason::CumulativeNotionalExceeded;
        }

        return RejectReason::None;
    }

    // Call once the order returned by a passing check() has actually been
    // forwarded downstream. Never call this for a rejected order — a
    // rejected order was never sent, so it must not count against the
    // running totals.
    void record(const ChildOrder& order) {
        cumulative_shares_ += order.shares;
        cumulative_notional_ += static_cast<unsigned __int128>(order.price) *
                                 static_cast<unsigned __int128>(order.shares);
    }

    bool tripped() const { return tripped_; }

    // The explicit, human-driven un-trip. Also re-zeroes the running
    // cumulative totals, not just the latch: leaving them at (or past) the
    // limit would make reset() a no-op in practice, since the very next
    // order would immediately re-trip the switch it was just supposed to
    // clear.
    void reset() {
        tripped_ = false;
        cumulative_shares_ = 0;
        cumulative_notional_ = 0;
    }

private:
    RiskLimits limits_;
    Price reference_price_ = 0;

    bool tripped_ = false;
    Shares cumulative_shares_ = 0;
    unsigned __int128 cumulative_notional_ = 0;
};

// The check-then-record pairing most callers actually want: drains `in`
// through gate, pushing whatever passes into `accepted` (and recording it
// against the gate's cumulative totals), leaving whatever doesn't out of
// `accepted` entirely. reasons_out, if supplied, receives one RejectReason
// per rejected order in the order encountered — its capacity matches
// ChildOrderQueue's own (kMaxChildOrders), so every rejection always has
// room; there is no separate bounds check because `in` itself can never
// hold more entries than that.
//
// Returns the number of orders rejected — accepted.size() already reports
// how many passed.
inline std::size_t apply(RiskGate& gate, const ChildOrderQueue& in, ChildOrderQueue& accepted,
                          std::array<RejectReason, kMaxChildOrders>* reasons_out = nullptr) {
    std::size_t rejected = 0;
    for (const auto& order : in) {
        const RejectReason reason = gate.check(order);
        if (reason == RejectReason::None) {
            gate.record(order);
            accepted.push(order);
            continue;
        }
        if (reasons_out != nullptr) {
            (*reasons_out)[rejected] = reason;
        }
        ++rejected;
    }
    return rejected;
}

}  // namespace exec
