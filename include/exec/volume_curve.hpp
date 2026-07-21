#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "exec/types.hpp"

// Fixed-bucket historical intraday volume-curve model: the classic U-shape
// seen across real equity trading days (heavy volume in the first bucket
// after the open, a mid-day lull, heavy again into the close). This is what
// Vwap now consults instead of a pure elapsed-time ramp -- see the header
// comment on exec::Vwap for how the two combine.
//
// This is a single canonical, stylized curve (illustrative U-shape weights,
// not fit from a specific empirical dataset -- this engine has no
// persistent historical-volume store to fit one from) shared by every Vwap
// instance, not something re-estimated per symbol or per day. kVolumeCurveBuckets
// buckets span whatever [start_ts, end_ts) a caller passes, evenly -- this is
// a *relative* shape, not a wall-clock trading-hours table, so it applies
// whether the caller's window is a full 6.5-hour session or a 10-minute
// clip of one.
namespace exec {

// 13 buckets == the 6.5-hour Nasdaq/NYSE cash session sliced into 30-minute
// bars, the standard granularity real intraday-volume curves are quoted at.
inline constexpr std::size_t kVolumeCurveBuckets = 13;

// Fixed-point scale VolumeCurve's cumulative table is expressed in:
// cumulative_weight(i) is "how many kCurveScale-ths of the day's total
// volume has printed by the end of bucket i". Vwap multiplies by
// total_shares and divides by this constant with the same
// widen-to-UInt128-then-divide pattern it already used for the old
// elapsed-time math (see exec/types.hpp's UInt128 comment) -- everything
// downstream of construction is integer only.
inline constexpr std::uint64_t kCurveScale = 1'000'000;

// Precomputes a fixed-point cumulative distribution table from a stylized
// U-shaped relative-weight curve. All floating point in this class is
// confined to the constructor -- it runs once, when a Vwap is constructed,
// not per message -- and the only thing stored is the resulting integer
// cumulative_ table. Nothing here is a runtime input: the shape is a fixed
// constant, so every VolumeCurve instance ends up holding the identical
// table. It is a genuine object (not e.g. a static local) so Vwap can hold
// it by value with no lifetime/init-order questions and so a future caller
// that DOES want to plug in a differently-shaped or per-symbol curve has an
// obvious seam to do it at.
class VolumeCurve {
public:
    VolumeCurve() {
        // Relative weights for each 30-minute bucket of a 6.5-hour session,
        // heaviest at the open and (slightly heavier still, reflecting the
        // modern closing-auction print) at the close, tapering to a
        // mid-day trough. These are illustrative proportions, not a fit
        // against real tape -- only their partial sums below matter, so
        // they need not sum to any particular total.
        constexpr std::array<double, kVolumeCurveBuckets> kRelativeWeights = {
            15.0, 9.0, 7.0, 6.0, 5.0, 4.5, 4.5, 5.0, 5.5, 6.0, 7.0, 9.0, 16.0,
        };

        double total = 0.0;
        for (double w : kRelativeWeights) total += w;

        double running = 0.0;
        for (std::size_t i = 0; i < kVolumeCurveBuckets; ++i) {
            running += kRelativeWeights[i];
            // Round to nearest rather than truncate: truncating would
            // systematically under-allocate every bucket and dump the
            // accumulated error onto whichever bucket happens to be last.
            cumulative_[i] = static_cast<std::uint64_t>(
                running / total * static_cast<double>(kCurveScale) + 0.5);
        }
        // Force the last bucket to land exactly on kCurveScale rather than
        // trusting float rounding to get there -- Vwap's done()/target_shares()
        // invariant (fully filled by end_ts) depends on this being exact.
        cumulative_[kVolumeCurveBuckets - 1] = kCurveScale;
    }

    // How many kCurveScale-ths of total volume have printed by the end of
    // bucket `i` (0-indexed). Out-of-range i clamps to the last bucket so
    // callers get the "fully elapsed" answer instead of undefined behavior.
    std::uint64_t cumulative_weight(std::size_t i) const {
        if (i >= kVolumeCurveBuckets) i = kVolumeCurveBuckets - 1;
        return cumulative_[i];
    }

private:
    std::array<std::uint64_t, kVolumeCurveBuckets> cumulative_{};
};

}  // namespace exec
