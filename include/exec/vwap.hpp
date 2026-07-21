#pragma once
#include <cstddef>
#include <stdexcept>

#include "exec/execution_strategy.hpp"
#include "exec/types.hpp"
#include "exec/volume_curve.hpp"

namespace exec {

// Parent order for a volume-weighted execution. Same field shape as
// TwapParams (see exec/twap.hpp) for consistency, minus the bin_ns slicing
// parameter — Vwap has no clock-driven schedule to pre-compute, it reacts
// to the tape against a historical volume curve instead (see below).
struct VwapParams {
    itch::Side side = itch::Side::Buy;
    Shares total_shares = 0;
    Timestamp start_ts = 0;
    Timestamp end_ts = 0;   // must be > start_ts
    OrderType child_type = OrderType::Limit;
    Price limit_price = 0;  // used when child_type == OrderType::Limit, else ignored
};

// Volume-weighted average price, driven by the tape rather than a clock.
// The participation target is read off a fixed historical intraday
// volume-curve model (exec::VolumeCurve, see exec/volume_curve.hpp) — the
// classic U-shape, heavy at the open and close, lighter mid-day — rather
// than a flat elapsed-time ramp:
//
//   target_shares(now) = total_shares * curve_fraction(now)
//
// where curve_fraction(now) is the cumulative share of the curve's volume
// that [start_ts, now] is expected to have printed, linearly interpolated
// within whichever of the curve's kVolumeCurveBuckets buckets `now` falls
// in (the same "volume is uniform within a slice" assumption Twap makes
// globally, just applied to one curve bucket instead of the whole
// session). The curve is a *relative* shape stretched across whatever
// [start_ts, end_ts) window is given — it is not a wall-clock
// trading-hours table, so a 10-minute window gets the same U-shaped
// front/back-loading a full session would, just compressed.
//
// Every trade tick inside [start_ts, end_ts] is an opportunity to catch up
// to that target: on each tick, Vwap sends max(0, target_shares(now) -
// shares_sent), clamped so cumulative sent never exceeds total_shares. A
// tick outside the window is a no-op — before start_ts there is nothing to
// catch up to, after end_ts the schedule has already run out.
//
// Unlike Twap, on_bbo_change_impl is a genuine no-op: this strategy has
// nothing to react to on a quote change, only on a print. It still exists so
// Vwap satisfies the same ExecutionStrategy interface as Twap and Pov and
// all three can share one dispatch loop.
//
// The per-message hot path (target_shares(), called from
// on_trade_tick_impl on every print) is entirely integer arithmetic — it
// only ever touches VolumeCurve's precomputed integer cumulative_ table,
// never a float. The one place this design genuinely needs floating point
// — turning the curve's hand-authored relative weights into that integer
// table — is confined to VolumeCurve's constructor, which runs once per
// Vwap construction, not per message; see exec/volume_curve.hpp for that
// code and rationale. target_shares() itself multiplies total_shares (up
// to 2^32-1) by curve/time quantities that can each span most of a trading
// day in nanoseconds, so those products are computed in UInt128 (see
// exec/types.hpp) before dividing back down to Shares — a plain
// std::uint64_t product overflows well within realistic inputs.
class Vwap : public ExecutionStrategy<Vwap> {
public:
    // Precondition: params.start_ts < params.end_ts. Violation throws
    // std::invalid_argument — a construction-time contract failure, not a
    // runtime state this class has to keep checking on every tick. Not
    // assert(): NDEBUG (set by every build configuration this project
    // ships, Release and RelWithDebInfo alike) would compile that check
    // away entirely — and bucket_position()/target_shares() below divide by
    // span == end_ts - start_ts, so an unchecked start_ts >= end_ts is a
    // division-by-zero (or unsigned-underflow-then-huge-divisor) away, not
    // just a semantically wrong schedule.
    explicit Vwap(const VwapParams& params) : params_(params) {
        if (params_.start_ts >= params_.end_ts)
            throw std::invalid_argument("Vwap: start_ts must be < end_ts");
    }

    bool done() const { return shares_sent_ >= params_.total_shares; }
    Shares shares_sent() const { return shares_sent_; }

    // Cumulative shares traded on the tape since start_ts, across all sides —
    // bookkeeping for observability, not an input to target_shares() (the
    // schedule is weighted against the fixed VolumeCurve model, not
    // against volume actually observed on this tape).
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
    // Which curve bucket `now` falls in, plus how far into that bucket it
    // is (as a remainder over `span`, so the caller never needs a separate
    // "bucket width" quantity that might not divide span evenly). Mirrors
    // Twap's bin math, just against kVolumeCurveBuckets instead of a
    // caller-supplied bin_ns.
    struct BucketPosition {
        std::size_t index;
        Timestamp remainder;  // out of `span`, i.e. fraction == remainder / span
    };

    static BucketPosition bucket_position(Timestamp elapsed, Timestamp span) {
        if (elapsed >= span) {
            return BucketPosition{kVolumeCurveBuckets - 1, span};
        }
        // Multiplying elapsed by kVolumeCurveBuckets before dividing by
        // span (not the other way around) keeps this exact for the same
        // reason the old elapsed-time math widened first — see
        // exec/types.hpp's UInt128 comment.
        const UInt128 scaled = static_cast<UInt128>(elapsed) * kVolumeCurveBuckets;
        std::size_t index = static_cast<std::size_t>(scaled / span);
        if (index >= kVolumeCurveBuckets) index = kVolumeCurveBuckets - 1;
        const Timestamp remainder = static_cast<Timestamp>(scaled % span);
        return BucketPosition{index, remainder};
    }

    // target_shares(now) = total_shares * curve_fraction(now), where
    // curve_fraction(now) is VolumeCurve's cumulative weight through the
    // end of the previous bucket, plus a linear-interpolation slice of the
    // current bucket's weight for how far `now` is into it. Every step is
    // integer, widened to UInt128 (see exec/types.hpp) before any divide,
    // so a large total_shares times a large curve/time quantity cannot
    // wrap a 64-bit accumulator before the division brings it back into
    // range.
    Shares target_shares(Timestamp now) const {
        const Timestamp elapsed = now - params_.start_ts;
        const Timestamp span = params_.end_ts - params_.start_ts;
        const BucketPosition pos = bucket_position(elapsed, span);

        const std::uint64_t cum_before =
            pos.index == 0 ? 0 : curve_.cumulative_weight(pos.index - 1);
        const std::uint64_t cum_after = curve_.cumulative_weight(pos.index);
        const std::uint64_t bucket_weight = cum_after - cum_before;

        const UInt128 within_bucket =
            static_cast<UInt128>(bucket_weight) * pos.remainder / span;
        const UInt128 curve_fraction = static_cast<UInt128>(cum_before) + within_bucket;

        const UInt128 target =
            static_cast<UInt128>(params_.total_shares) * curve_fraction / kCurveScale;
        return target >= params_.total_shares
                   ? params_.total_shares
                   : static_cast<Shares>(target);
    }

    VwapParams params_;
    VolumeCurve curve_;
    Shares shares_sent_ = 0;
    Shares market_shares_seen_ = 0;
    ChildOrderQueue queue_;
};

}  // namespace exec
