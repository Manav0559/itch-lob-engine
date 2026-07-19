#pragma once
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "exec/execution_strategy.hpp"
#include "exec/types.hpp"

namespace exec {

// Parent order + risk-aware trade trajectory, per Almgren & Chriss's
// optimal-execution model. Same field shape as TwapParams plus the three
// parameters that trade impact off against price risk: a risk-averse
// trader (risk_aversion > 0) front-loads more of the order into the early
// bins, trading faster (and eating more temporary impact) in exchange for
// less time exposed to price volatility over the remaining horizon.
struct AlmgrenChrissParams {
    itch::Side side = itch::Side::Buy;
    Shares total_shares = 0;
    Timestamp start_ts = 0;
    Timestamp end_ts = 0;  // must be > start_ts
    Timestamp bin_ns = 1;  // slice interval; bin_count = ceil((end_ts-start_ts)/bin_ns)
    double risk_aversion = 0.0;       // lambda >= 0; 0 degenerates to a uniform (Twap) schedule
    double volatility = 0.0;          // sigma, per unit time (same time unit as bin_ns/tau)
    double impact_coefficient = 1.0;  // eta > 0, temporary market impact coefficient
    OrderType child_type = OrderType::Limit;
    Price limit_price = 0;  // used when child_type == OrderType::Limit, else ignored
};

// Almgren-Chriss optimal-execution trajectory: precomputes, at construction,
// the trade schedule that minimizes expected impact cost plus
// risk_aversion times the variance of execution cost. Unlike Twap's
// uniform split, the resulting schedule is front-loaded for risk_aversion
// > 0 — bigger clips early, tapering toward the end — and recovers Twap's
// uniform schedule exactly as risk_aversion -> 0.
//
// All of the transcendental math (acosh/sinh) runs once, in the
// constructor, against doubles — the same construction-time-only use of
// floating point as LadderBook's window_pct (see ladder_book.hpp). The
// schedule is rounded to integer Shares once, up front, so advance() is
// pure integer bin-crossing logic on the hot path, identical in spirit to
// Twap's.
class AlmgrenChriss : public ExecutionStrategy<AlmgrenChriss> {
public:
    // Precondition: params.start_ts < params.end_ts, params.bin_ns > 0,
    // params.risk_aversion >= 0, params.volatility >= 0,
    // params.impact_coefficient > 0, and the resulting bin_count fits
    // within kMaxScheduleBins. Violations are a construction-time contract
    // failure (asserted), not a runtime state this class has to keep
    // checking on every advance() call.
    explicit AlmgrenChriss(const AlmgrenChrissParams& params);

    // Same bin-crossing semantics as Twap::advance: pushes one child order
    // per bin boundary crossed since the last call, using the precomputed
    // integer schedule. Returns the number of child orders pushed this
    // call.
    std::size_t advance(Timestamp now);

    bool done() const { return next_bin_ >= bin_count_; }
    std::size_t bin_count() const { return bin_count_; }

    const ChildOrderQueue& child_orders() const { return queue_; }
    ChildOrderQueue& child_orders() { return queue_; }

    void on_bbo_change_impl(const Bbo&) {}
    void on_trade_tick_impl(const TradeTick&) {}

private:
    AlmgrenChrissParams params_;
    std::array<Shares, kMaxScheduleBins> schedule_{};  // schedule_[i] = shares due at bin i
    std::size_t bin_count_ = 0;
    std::size_t next_bin_ = 0;
    ChildOrderQueue queue_;
};

inline AlmgrenChriss::AlmgrenChriss(const AlmgrenChrissParams& params) : params_(params) {
    assert(params.start_ts < params.end_ts);
    assert(params.bin_ns > 0);
    assert(params.risk_aversion >= 0.0);
    assert(params.volatility >= 0.0);
    assert(params.impact_coefficient > 0.0);
    const Timestamp span = params.end_ts - params.start_ts;
    bin_count_ = static_cast<std::size_t>((span + params.bin_ns - 1) / params.bin_ns);
    assert(bin_count_ > 0 && bin_count_ <= kMaxScheduleBins);

    const double tau = static_cast<double>(params.bin_ns);
    const double horizon = tau * static_cast<double>(bin_count_);
    const std::size_t n = bin_count_;

    double kappa = 0.0;
    if (params.risk_aversion > 0.0) {
        const double acosh_arg =
            1.0 + (params.risk_aversion * params.volatility * params.volatility * tau * tau) /
                      (2.0 * params.impact_coefficient);
        kappa = std::acosh(acosh_arg) / tau;
    }

    // kappa*horizon governs how peaked the trajectory is; below this
    // threshold, sinh(kappa*(T-j*tau))/sinh(kappa*T) is a 0/0 in floating
    // point (numerator and denominator both collapse to zero together as
    // kappa -> 0), so fall back to the exact uniform split the formula
    // degenerates to in that limit rather than let the ratio go unstable.
    constexpr double kMinKappaHorizon = 1e-6;

    std::array<double, kMaxScheduleBins> shares_to_trade{};
    if (kappa * horizon < kMinKappaHorizon) {
        const double uniform = static_cast<double>(params.total_shares) / static_cast<double>(n);
        for (std::size_t i = 0; i < n; ++i) shares_to_trade[i] = uniform;
    } else {
        const double denom = std::sinh(kappa * horizon);
        double remaining_prev = static_cast<double>(params.total_shares);  // remaining at bin 0
        for (std::size_t j = 1; j <= n; ++j) {
            const double t_remaining = horizon - static_cast<double>(j) * tau;
            const double remaining =
                static_cast<double>(params.total_shares) * std::sinh(kappa * t_remaining) / denom;
            shares_to_trade[j - 1] = remaining_prev - remaining;
            remaining_prev = remaining;
        }
    }

    // Round to integer shares once, then push the rounding remainder onto
    // the last bin so the integer schedule still sums exactly to
    // total_shares — the same "don't drop the remainder" discipline Twap
    // applies to its own integer division.
    std::int64_t rounded_sum = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto rounded = static_cast<std::int64_t>(std::llround(shares_to_trade[i]));
        schedule_[i] = static_cast<Shares>(rounded);
        rounded_sum += rounded;
    }
    const std::int64_t diff = static_cast<std::int64_t>(params.total_shares) - rounded_sum;
    const std::int64_t adjusted_last = static_cast<std::int64_t>(schedule_[n - 1]) + diff;
    assert(adjusted_last >= 0);
    schedule_[n - 1] = static_cast<Shares>(adjusted_last);
}

inline std::size_t AlmgrenChriss::advance(Timestamp now) {
    std::size_t pushed = 0;
    while (next_bin_ < bin_count_) {
        const Timestamp boundary = params_.start_ts + (next_bin_ + 1) * params_.bin_ns;
        if (boundary > now) break;
        if (queue_.full()) break;
        queue_.push(ChildOrder{now, params_.side, params_.child_type,
                                params_.child_type == OrderType::Limit ? params_.limit_price
                                                                        : Price{0},
                                schedule_[next_bin_]});
        ++next_bin_;
        ++pushed;
    }
    return pushed;
}

}  // namespace exec
