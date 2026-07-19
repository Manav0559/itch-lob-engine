#pragma once
#include <cstdint>

#include "exec/execution_strategy.hpp"
#include "exec/types.hpp"

namespace exec {

// Parent order for a percentage-of-volume execution. Unlike Twap, Pov has no
// natural end condition from a schedule — it fires one child per observed
// trade for as long as the tape prints — so max_shares is a hard budget cap,
// not an optimization. end_ts is an optional second stop: 0 means "ignore,
// budget is the only limit" (a real ITCH session never starts at ts == 0).
struct PovParams {
    itch::Side side = itch::Side::Buy;
    Shares max_shares = 0;
    std::uint32_t participation_bps = 0;  // e.g. 1000 == 10%, integer basis points
    Timestamp end_ts = 0;                 // 0 == unused, budget is the only stop
};

// Percentage-of-volume: tick-reactive like Vwap, but targets a fixed slice
// of EACH observed trade's volume rather than a time-weighted total. Every
// trade on the tape (regardless of side — Pov trades off total printed
// volume, same convention as a real POV algo) produces at most one child
// order sized at participation_bps of that trade's shares, clamped so the
// running total never exceeds max_shares. Book-agnostic by design —
// on_bbo_change_impl is a no-op, present only so Pov satisfies
// exec::ExecutionStrategy.
class Pov : public ExecutionStrategy<Pov> {
public:
    explicit Pov(const PovParams& params) : params_(params) {}

    void on_bbo_change_impl(const Bbo&) {}

    // Pushes one child order sized at participation_bps of tick.shares,
    // clamped to whatever budget remains. No-op once the budget is
    // exhausted, past end_ts (if set), or the resulting slice rounds to
    // zero shares.
    void on_trade_tick_impl(const TradeTick& tick) {
        if (done()) return;
        if (params_.end_ts != 0 && tick.ts > params_.end_ts) return;

        // Both operands are uint32_t-range; promote to uint64_t before the
        // multiply so a large print (e.g. a block trade) times a high
        // participation rate can't wrap Shares before the /10000 brings it
        // back down.
        const std::uint64_t raw =
            static_cast<std::uint64_t>(tick.shares) * static_cast<std::uint64_t>(params_.participation_bps) / 10'000;

        const Shares remaining = params_.max_shares - shares_sent_;
        const Shares this_slice = raw > remaining ? remaining : static_cast<Shares>(raw);
        if (this_slice == 0) return;

        queue_.push(ChildOrder{
            .ts = tick.ts,
            .side = params_.side,
            .type = OrderType::Market,
            .price = 0,
            .shares = this_slice,
        });
        shares_sent_ += this_slice;
    }

    bool done() const { return shares_sent_ >= params_.max_shares; }
    Shares shares_sent() const { return shares_sent_; }
    Shares remaining() const { return params_.max_shares - shares_sent_; }

    const ChildOrderQueue& child_orders() const { return queue_; }
    ChildOrderQueue& child_orders() { return queue_; }

private:
    PovParams params_;
    Shares shares_sent_ = 0;
    ChildOrderQueue queue_;
};

}  // namespace exec
