#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "exec/types.hpp"

// A risk layer that sits between "a strategy decided to trade" and "the order
// actually goes out." Nothing upstream of this file checks a ChildOrder for
// sanity — Twap/Vwap/Pov/AlmgrenChriss will happily emit an order of any size
// at any price, because sizing/pricing is their job, not risk's. This closes
// that gap: every real trading system has something exactly like RiskGate
// between a strategy's ChildOrderQueue output and wherever orders actually go
// next (FillSimulator in this codebase, an OMS in a live system).
//
// Same allocation/exception discipline as the rest of exec/: RiskGate holds
// only fixed-width scalars, check()/record() never throw, and the apply()
// helper below only ever touches fixed-capacity ChildOrderQueue/std::array
// storage the caller already owns.
namespace exec {

// Per-session risk limits. Every limit here is a hard ceiling: crossing a
// per-order one rejects that order, crossing a cumulative one rejects the
// order AND latches the kill switch (see RiskGate below).
struct RiskLimits {
    Shares max_order_shares = 0;           // largest single ChildOrder, in shares
    std::uint64_t max_order_notional = 0;  // largest single ChildOrder, price * shares
    // Max allowed deviation of a Limit order's price from the reference price,
    // in basis points of the reference price. Market orders carry no price
    // (see ChildOrder::price — "ignored when type == OrderType::Market"), so
    // there is nothing to collar them against; the check is skipped for them,
    // not applied against some fabricated price.
    std::uint32_t price_collar_bps = 0;
    Shares max_cumulative_shares = 0;           // running total across every recorded order
    std::uint64_t max_cumulative_notional = 0;  // running total across every recorded order
};

// Why an order was rejected, not just whether. A bool gate only tells an
// operator "something's wrong today"; the reason is what actually lets a
// human (or an automated alert) tell a fat-fingered size apart from a
// runaway strategy tripping the cumulative budget.
enum class RejectReason : std::uint8_t {
    None,
    OrderTooLarge,
    NotionalTooLarge,
    PriceOutOfCollar,
    CumulativeSharesExceeded,
    CumulativeNotionalExceeded,
    KillSwitchTripped,
};

// Gates a strategy's ChildOrders against RiskLimits before they're allowed
// downstream.
//
// check() vs. record() is a deliberate split, not two halves of one method:
// check() only ever reads state, so a caller can ask "would this pass?"
// (a what-if / speculative check) without committing anything, and can
// safely call check() more than once for the same order. record() is the
// one place cumulative totals move, and it's only ever meant to be called
// once check() has already said None and the order has actually gone out —
// mirroring how FillSimulator's caller feeds state updates before evaluating
// a fill (see fill_sim.hpp), rather than folding "evaluate" and "commit" into
// one call that can't be un-done if the send fails downstream.
//
// Kill switch semantics: a cumulative breach (shares or notional) doesn't
// just reject the order that caused it — it latches the gate into a tripped
// state that rejects every subsequent check() with KillSwitchTripped,
// regardless of that later order's own size or price. This is deliberately
// NOT a threshold that silently clears once volume drops back under the
// limit: a real kill switch requires a human/ops action to come back online,
// which is what reset() models. A per-order breach (size/notional/collar) is
// NOT itself a kill-switch event — those reject just the one order, since a
// single fat-fingered order isn't evidence the whole session is unsafe the
// way a blown cumulative budget is.
class RiskGate {
public:
    // reference_price follows the same "0 == not set" sentinel as Bbo's own
    // price fields in types.hpp (a valid ITCH price is never zero): until a
    // caller feeds an initial reference price — from the BBO, an arrival
    // price, whatever the caller's convention is — there is nothing to
    // collar a Limit order's price against, so the collar check is skipped
    // rather than comparing against a fabricated zero reference.
    explicit RiskGate(const RiskLimits& limits, Price reference_price = 0)
        : limits_(limits), reference_price_(reference_price) {}

    // Caller updates this over the session (e.g. off the current BBO or an
    // arrival price) so the collar check tracks a moving market rather than
    // a single price fixed at construction.
    void set_reference_price(Price p) { reference_price_ = p; }
    Price reference_price() const { return reference_price_; }

    // Evaluates order against every per-order limit (size, notional, collar
    // for Limit orders) and the cumulative limits, returning the FIRST
    // violated reason or None. Does NOT update the cumulative running
    // totals on a pass — call record() for that once the order is actually
    // sent (see class comment). It DOES latch the kill switch the moment it
    // detects a cumulative breach, since that's the instant this order (and
    // every one after it) becomes a reject — there is no separate "detect"
    // step before the latch takes effect.
    //
    // Checked in this order, not enum declaration order: an already-tripped
    // gate must reject a tiny, otherwise-flawless order with
    // KillSwitchTripped rather than walking through checks it would
    // otherwise pass, so the latch is tested first, not last.
    RejectReason check(const ChildOrder& order) {
        if (tripped_) return RejectReason::KillSwitchTripped;

        if (order.shares > limits_.max_order_shares) return RejectReason::OrderTooLarge;

        // price and shares are each uint32_t; their product fits in
        // uint64_t with room to spare (max 4294967295^2 < 2^64 - 1), unlike
        // FillSimulator's notional_ accumulator (fill_sim.hpp) which sums
        // that product across many fills and needs unsigned __int128 to
        // stay safe over a full day. A single order's notional doesn't need
        // that — same widen-before-multiply discipline as exec::Vwap/Pov,
        // just to the narrower width this one multiply actually requires.
        const std::uint64_t order_notional =
            static_cast<std::uint64_t>(order.price) * static_cast<std::uint64_t>(order.shares);
        if (order_notional > limits_.max_order_notional) return RejectReason::NotionalTooLarge;

        if (order.type == OrderType::Limit && reference_price_ != 0) {
            const Price diff = order.price >= reference_price_ ? order.price - reference_price_
                                                                 : reference_price_ - order.price;
            const std::uint64_t allowed_deviation = static_cast<std::uint64_t>(reference_price_) *
                                                     static_cast<std::uint64_t>(limits_.price_collar_bps) / 10'000;
            if (static_cast<std::uint64_t>(diff) > allowed_deviation) return RejectReason::PriceOutOfCollar;
        }

        // Cumulative checks are against what the running total WOULD become
        // if this order were recorded, not the total as it stands now —
        // otherwise the order that actually crosses the limit would pass
        // (since the total only crosses it after this order), and the very
        // next one would trip instead, one order too late.
        if (cumulative_shares_ + order.shares > static_cast<std::uint64_t>(limits_.max_cumulative_shares)) {
            tripped_ = true;
            return RejectReason::CumulativeSharesExceeded;
        }
        if (cumulative_notional_ + static_cast<unsigned __int128>(order_notional) >
            static_cast<unsigned __int128>(limits_.max_cumulative_notional)) {
            tripped_ = true;
            return RejectReason::CumulativeNotionalExceeded;
        }

        return RejectReason::None;
    }

    // Updates the running cumulative totals for an order that already
    // passed check(). Never itself rejects or trips anything — check()
    // already guaranteed this order's shares/notional fit under the
    // cumulative limits before this call, so record() is pure bookkeeping,
    // not a second gate.
    void record(const ChildOrder& order) {
        cumulative_shares_ += order.shares;
        cumulative_notional_ +=
            static_cast<unsigned __int128>(order.price) * static_cast<unsigned __int128>(order.shares);
    }

    bool tripped() const { return tripped_; }

    // Un-trips the kill switch for a human/ops action — there is no
    // automatic self-healing (see class comment). Also zeroes the
    // cumulative running totals: leaving them at their already-over-limit
    // values would make reset() a no-op in practice, since the very next
    // check() would immediately re-trip on a total that never actually
    // came down. An ops reset is modeled here as starting a fresh
    // accounting window, the same way a real desk's kill-switch reset
    // typically comes with a fresh session/limit window rather than
    // resuming against a stale, already-blown total.
    void reset() {
        tripped_ = false;
        cumulative_shares_ = 0;
        cumulative_notional_ = 0;
    }

private:
    RiskLimits limits_;
    Price reference_price_ = 0;

    bool tripped_ = false;
    // Widened past the RiskLimits field widths (Shares / uint64_t) for the
    // running accumulation itself, not just the per-order multiply: these
    // sum across every order recorded for the life of the gate, and adding
    // into a same-width accumulator right at its ceiling is exactly the
    // silent-wraparound bug the rest of exec/ widens against before
    // multiplying (see exec::Vwap, exec::Pov, FillSimulator::notional_).
    std::uint64_t cumulative_shares_ = 0;
    unsigned __int128 cumulative_notional_ = 0;
};

static_assert(std::is_trivially_copyable_v<RiskLimits>);

// Drains `in` through gate, pushing every order that passes check() into
// `accepted` (and calling record() for it so the cumulative totals reflect
// that it was actually sent), in tape order. This is the piece a caller
// (a future replay/live binary sitting a RiskGate between a strategy's
// ChildOrderQueue and FillSimulator, or just a test) actually reaches for
// rather than hand-rolling the check/record loop itself.
//
// reasons_out, if non-null, is filled parallel to `in`: reasons_out->at(i)
// is the RejectReason for in's i-th order (RejectReason::None for one that
// was accepted). Indices at or past in.size() are left untouched. A
// separate `rejected` queue isn't part of this signature because `in` itself
// already holds every rejected order — a caller that wants the rejected
// ChildOrders back can walk `in` alongside reasons_out and keep the ones
// where the reason isn't None.
//
// Returns the number of orders accepted (i.e. how many made it into
// `accepted`), which is the one number most callers actually want to log or
// assert on.
inline std::size_t apply(RiskGate& gate, const ChildOrderQueue& in, ChildOrderQueue& accepted,
                          std::array<RejectReason, kMaxChildOrders>* reasons_out = nullptr) {
    std::size_t accepted_count = 0;
    std::size_t i = 0;
    for (const ChildOrder& order : in) {
        const RejectReason reason = gate.check(order);
        if (reasons_out != nullptr) {
            (*reasons_out)[i] = reason;
        }
        if (reason == RejectReason::None && accepted.push(order)) {
            gate.record(order);
            ++accepted_count;
        }
        ++i;
    }
    return accepted_count;
}

}  // namespace exec
