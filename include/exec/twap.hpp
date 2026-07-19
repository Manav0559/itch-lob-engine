#pragma once
#include <array>
#include <cassert>
#include <cstddef>

#include "exec/execution_strategy.hpp"
#include "exec/types.hpp"

namespace exec {

// Parent order + schedule for a time-sliced execution. Every field is a
// plain integer on the same clock/tick units as the ITCH decoders (see
// exec::Timestamp, exec::Price) so a schedule can be driven directly off
// ITCH header timestamps with no unit conversion on the hot path.
struct TwapParams {
    itch::Side side = itch::Side::Buy;
    Shares total_shares = 0;
    Timestamp start_ts = 0;
    Timestamp end_ts = 0;   // must be > start_ts
    Timestamp bin_ns = 1;   // slice interval; bin_count = ceil((end_ts-start_ts)/bin_ns)
    OrderType child_type = OrderType::Limit;
    Price limit_price = 0;  // used when child_type == OrderType::Limit, else ignored
};

// Time-weighted average price: slices total_shares into bin_count roughly
// equal child orders at regular bin_ns intervals from start_ts to end_ts.
// Volume- and book-agnostic by design — on_bbo_change_impl/on_trade_tick_impl
// are no-ops, present only so Twap satisfies exec::ExecutionStrategy.
//
// The schedule lives in a std::array<Shares, kMaxScheduleBins> sized at
// compile time (see exec::kMaxScheduleBins) — a params.bin_ns fine enough
// to need more bins than that is a construction-time error, not something
// that grows a container at runtime. There is no dynamic allocation
// anywhere in this class, construction included.
class Twap : public ExecutionStrategy<Twap> {
public:
    // Precondition: params.start_ts < params.end_ts, params.bin_ns > 0, and
    // the resulting bin_count fits within kMaxScheduleBins. Violations are
    // a construction-time contract failure (asserted), not a runtime state
    // this class has to keep checking on every advance() call.
    explicit Twap(const TwapParams& params);

    // Advances the schedule to `now`, pushing one child order into
    // child_orders() for every bin boundary crossed since the last call
    // (ordinarily zero or one per call; more than one if advance() is
    // called less often than bin_ns). Returns the number of child orders
    // pushed this call — 0 once done() is true, or if child_orders() is
    // already full.
    std::size_t advance(Timestamp now);

    bool done() const { return next_bin_ >= bin_count_; }
    std::size_t bin_count() const { return bin_count_; }

    const ChildOrderQueue& child_orders() const { return queue_; }
    ChildOrderQueue& child_orders() { return queue_; }

    void on_bbo_change_impl(const Bbo&) {}
    void on_trade_tick_impl(const TradeTick&) {}

private:
    TwapParams params_;
    std::array<Shares, kMaxScheduleBins> schedule_{};  // schedule_[i] = shares due at bin i
    std::size_t bin_count_ = 0;
    std::size_t next_bin_ = 0;
    ChildOrderQueue queue_;
};

inline Twap::Twap(const TwapParams& params) : params_(params) {
    assert(params.start_ts < params.end_ts);
    assert(params.bin_ns > 0);
    const Timestamp span = params.end_ts - params.start_ts;
    bin_count_ = static_cast<std::size_t>((span + params.bin_ns - 1) / params.bin_ns);
    assert(bin_count_ > 0 && bin_count_ <= kMaxScheduleBins);

    // Split total_shares as evenly as possible: every bin gets the integer
    // quotient, and the first `remainder` bins take one extra share each so
    // the sum lands on total_shares exactly rather than losing the remainder
    // to truncation.
    const Shares base = params.total_shares / static_cast<Shares>(bin_count_);
    const Shares remainder = params.total_shares % static_cast<Shares>(bin_count_);
    for (std::size_t i = 0; i < bin_count_; ++i) {
        schedule_[i] = base + (i < remainder ? 1 : 0);
    }
}

inline std::size_t Twap::advance(Timestamp now) {
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
