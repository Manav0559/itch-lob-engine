#pragma once
#include <cassert>

#include "exec/execution_strategy.hpp"
#include "exec/types.hpp"

namespace exec {

// Parent order for a volume-weighted execution. Same field shape as
// TwapParams (see exec/twap.hpp) for consistency, minus the bin_ns slicing
// parameter — Vwap has no schedule to pre-compute, it reacts to the tape.
struct VwapParams {
    itch::Side side = itch::Side::Buy;
    Shares total_shares = 0;
    Timestamp start_ts = 0;
    Timestamp end_ts = 0;   // must be > start_ts
    OrderType child_type = OrderType::Limit;
    Price limit_price = 0;  // used when child_type == OrderType::Limit, else ignored
};

// Volume-weighted average price, driven by the tape rather than a clock.
// This engine has no historical volume curve to participate against, so the
// participation target is elapsed-time-weighted instead:
//
//   target_shares(now) = total_shares * (now - start_ts) / (end_ts - start_ts)
//
// Every trade tick inside [start_ts, end_ts] is an opportunity to catch up to
// that target: on each tick, Vwap sends max(0, target_shares(now) -
// shares_sent), clamped so cumulative sent never exceeds total_shares. A
// tick outside the window is a no-op — before start_ts there is nothing to
// catch up to, after end_ts the schedule has already run out.
//
// Unlike Twap, on_bbo_change_impl is a genuine no-op: this strategy has
// nothing to react to on a quote change, only on a print. It still exists so
// Vwap satisfies the same ExecutionStrategy interface as Twap and Pov and
// all three can share one dispatch loop.
//
// All arithmetic is integer: target_shares(now) multiplies total_shares (up
// to 2^32-1) by an elapsed-time delta that can itself span most of a
// trading day in nanoseconds, so the product is computed in UInt128 (see
// exec/types.hpp) before dividing back down to Shares — a plain
// std::uint64_t product overflows well within realistic inputs.
class Vwap : public ExecutionStrategy<Vwap> {
public:
    // Precondition: params.start_ts < params.end_ts. Violation is a
    // construction-time contract failure (asserted), not a runtime state
    // this class has to keep checking on every tick.
    explicit Vwap(const VwapParams& params) : params_(params) {
        assert(params_.start_ts < params_.end_ts);
    }

    bool done() const { return shares_sent_ >= params_.total_shares; }
    Shares shares_sent() const { return shares_sent_; }

    // Cumulative shares traded on the tape since start_ts, across all sides —
    // bookkeeping for observability, not an input to target_shares() (this
    // engine has no historical curve to weight it against).
    Shares market_shares_seen() const { return market_shares_seen_; }

    const ChildOrderQueue& child_orders() const { return queue_; }
    ChildOrderQueue& child_orders() { return queue_; }

    void on_bbo_change_impl(const Bbo&) {}

    void on_trade_tick_impl(const TradeTick& tick) {
        if (tick.ts < params_.start_ts || tick.ts > params_.end_ts) return;
        market_shares_seen_ += tick.shares;
        if (done()) return;

        const Shares target = target_shares(tick.ts);
        if (target <= shares_sent_) return;

        const Shares remaining = params_.total_shares - shares_sent_;
        const Shares behind = target - shares_sent_;
        const Shares to_send = behind < remaining ? behind : remaining;
        if (to_send == 0) return;

        const ChildOrder child{tick.ts, params_.side, params_.child_type,
                                params_.limit_price, to_send};
        if (queue_.push(child)) {
            shares_sent_ += to_send;
        }
    }

private:
    // target_shares(now) = total_shares * (now - start_ts) / (end_ts -
    // start_ts), widened to UInt128 (see exec/types.hpp) for the multiply so
    // a large total_shares times a large elapsed-time delta cannot wrap a
    // 64-bit accumulator before the division brings it back into Shares range.
    Shares target_shares(Timestamp now) const {
        const Timestamp elapsed = now - params_.start_ts;
        const Timestamp span = params_.end_ts - params_.start_ts;
        const UInt128 target = static_cast<UInt128>(params_.total_shares) * elapsed / span;
        return target >= params_.total_shares
                   ? params_.total_shares
                   : static_cast<Shares>(target);
    }

    VwapParams params_;
    Shares shares_sent_ = 0;
    Shares market_shares_seen_ = 0;
    ChildOrderQueue queue_;
};

}  // namespace exec
