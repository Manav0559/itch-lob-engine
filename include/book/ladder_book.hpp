#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "book/order_book.hpp"
#include "itch/messages.hpp"

namespace book {

// Same order-id book as OrderBook — same hash-map locator, same mutator
// semantics — but the two std::map ladders are replaced with flat arrays
// indexed by (price - min_price_) / tick_size_. A single level's lookup and
// mutation become O(1) array indexing instead of O(log levels) tree work, at
// the cost of a bounded price range fixed at construction: add() rejects a
// price outside that range the same way it rejects a duplicate ref (false
// return, no partial state), rather than growing the ladder to fit.
//
// tick_size_ is a real constraint, not just a memory-vs-range knob: prices
// must land exactly on the (min_price_, min_price_ + tick_size_, ...) grid,
// or two distinct prices could floor-divide into the same array slot and
// silently merge two real price levels into one. add() rejects a
// non-grid-aligned price the same way it rejects an out-of-range one — see
// pipeline::BookTraits<LadderBook> (dispatch_to_book.hpp) for the tick_size
// production actually constructs this with against real ITCH data.
//
// The window defaults to +/-20% around the caller-supplied reference price,
// which comfortably covers a normal trading day for one name. A name that
// gaps or halts past that band needs a wider window_pct passed explicitly —
// there is no way to resize after construction. This bounded-range tradeoff,
// weighed against std::map's unbounded range, is exactly what the benchmark
// harness measures OrderBook against.
//
// best_bid()/best_ask() never scan the ladder: a "best occupied index" per
// side is tracked incrementally. It advances/retreats past emptied levels
// only when the current best itself empties out, not on every call.
class LadderBook {
public:
    explicit LadderBook(std::uint32_t base_price, std::uint32_t tick_size = 1,
                         double window_pct = 0.20)
        : LadderBook(tick_size == 0 ? 1 : tick_size,
                     snap_to_tick(window_low(base_price, window_pct), tick_size == 0 ? 1 : tick_size),
                     window_high(base_price, window_pct)) {}

    bool add(std::uint64_t ref, itch::Side side, std::uint32_t shares, std::uint32_t price) {
        // Rejected the same way an out-of-range price is: false, no partial
        // state. idx() below is integer division by tick_size_ — a price
        // that isn't grid-aligned to min_price_ would floor-divide into
        // whatever slot is nearest, silently colliding with a real adjacent
        // price level instead of getting its own. That's harmless for a
        // benchmark seeded with grid-aligned synthetic prices (this ladder's
        // original use), but a real ITCH feed can carry sub-penny prices
        // (Reg NMS Rule 612 permits them under $1), so this check matters
        // now that LadderBook is wired into production against real files.
        if (!in_range(price) || (price - min_price_) % tick_size_ != 0) return false;
        const auto [it, inserted] = orders_.try_emplace(ref, Order{shares, price, side});
        if (!inserted) return false;  // duplicate ref: never double-count a level
        const std::size_t i = idx(price);
        if (side == itch::Side::Buy) {
            Level& lvl = bids_[i];
            if (lvl.orders == 0) ++bid_level_count_;
            lvl.shares += shares;
            lvl.orders += 1;
            if (!best_bid_idx_ || i > *best_bid_idx_) best_bid_idx_ = i;  // higher index = higher price
        } else {
            Level& lvl = asks_[i];
            if (lvl.orders == 0) ++ask_level_count_;
            lvl.shares += shares;
            lvl.orders += 1;
            if (!best_ask_idx_ || i < *best_ask_idx_) best_ask_idx_ = i;  // lower index = lower price
        }
        return true;
    }

    // 'E'/'C' and 'X' have identical book-keeping: shrink the order in place,
    // drop it (and its level, if emptied) once fully consumed.
    bool execute(std::uint64_t ref, std::uint32_t shares) { return reduce(ref, shares); }
    bool cancel(std::uint64_t ref, std::uint32_t shares) { return reduce(ref, shares); }

    bool remove(std::uint64_t ref) {
        const auto it = orders_.find(ref);
        if (it == orders_.end()) return false;
        const Order o = it->second;
        orders_.erase(it);
        level_sub(o.side, o.price, o.shares, /*order_gone=*/true);
        return true;
    }

    bool replace(std::uint64_t orig, std::uint64_t next, std::uint32_t shares,
                 std::uint32_t price) {
        const auto it = orders_.find(orig);
        if (it == orders_.end()) return false;
        const itch::Side side = it->second.side;  // 'U' carries no side field
        remove(orig);
        return add(next, side, shares, price);
    }

    std::optional<Quote> best_bid() const {
        if (!best_bid_idx_) return std::nullopt;
        const Level& l = bids_[*best_bid_idx_];
        return Quote{price_at(*best_bid_idx_), l.shares};
    }
    std::optional<Quote> best_ask() const {
        if (!best_ask_idx_) return std::nullopt;
        const Level& l = asks_[*best_ask_idx_];
        return Quote{price_at(*best_ask_idx_), l.shares};
    }

    std::size_t open_orders() const { return orders_.size(); }
    std::size_t bid_levels() const { return bid_level_count_; }
    std::size_t ask_levels() const { return ask_level_count_; }

private:
    struct Order {
        std::uint32_t shares;
        std::uint32_t price;
        itch::Side side;
    };

    LadderBook(std::uint32_t tick_size, std::uint32_t min_price, std::uint32_t max_price)
        : tick_size_(tick_size),
          min_price_(min_price),
          max_price_(check_range(min_price, max_price)),
          num_ticks_(static_cast<std::size_t>(max_price_ - min_price_) / tick_size_ + 1),
          bids_(num_ticks_),
          asks_(num_ticks_) {}

    // window_low/window_high now saturate rather than wrap (see above), so
    // this should never fire in practice — kept as a hard, non-assert
    // invariant check (throws, survives NDEBUG) rather than trusting the
    // arithmetic silently, since a uint32_t underflow here would otherwise
    // turn into an attempted multi-gigabyte vector allocation, not a clean
    // failure.
    static std::uint32_t check_range(std::uint32_t min_price, std::uint32_t max_price) {
        if (max_price < min_price)
            throw std::invalid_argument("LadderBook: max_price must be >= min_price");
        return max_price;
    }

    // window_low's own arithmetic (a float multiply then truncate) gives no
    // guarantee the result lands on the tick grid, even when base_price and
    // tick_size both do — snapping down here, once, at construction, is what
    // makes every later grid-aligned real price satisfy add()'s
    // (price - min_price_) % tick_size_ == 0 check. Anchoring the grid to an
    // absolute tick_size_ multiple (not to base_price specifically) is also
    // why two different LadderBooks with the same tick_size_ agree on where
    // the grid lines fall, even with different base prices.
    static std::uint32_t snap_to_tick(std::uint32_t price, std::uint32_t tick_size) {
        return (price / tick_size) * tick_size;
    }

    // Computed in uint64_t and saturated at UINT32_MAX rather than plain
    // uint32_t arithmetic: base_price is caller-supplied (ultimately
    // wire-derived — see pipeline::BookTraits<LadderBook>), and
    // base_price + base_price*window_pct overflows uint32_t for any
    // base_price past roughly UINT32_MAX/(1+window_pct). An unsaturated
    // overflow here would wrap max_price_ below min_price_, and the
    // constructor's (max_price_ - min_price_) computation is unsigned — it
    // would underflow to a huge value and attempt a multi-hundred-MB
    // vector allocation per LadderBook instead of failing predictably.
    static std::uint32_t window_low(std::uint32_t base_price, double window_pct) {
        const auto window = static_cast<std::uint64_t>(static_cast<double>(base_price) * window_pct);
        return window < base_price ? static_cast<std::uint32_t>(base_price - window) : 0;
    }
    static std::uint32_t window_high(std::uint32_t base_price, double window_pct) {
        const auto window = static_cast<std::uint64_t>(static_cast<double>(base_price) * window_pct);
        const std::uint64_t high = static_cast<std::uint64_t>(base_price) + window;
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(high, UINT32_MAX));
    }

    bool in_range(std::uint32_t price) const {
        return price >= min_price_ && price <= max_price_;
    }
    std::size_t idx(std::uint32_t price) const {
        return static_cast<std::size_t>((price - min_price_) / tick_size_);
    }
    std::uint32_t price_at(std::size_t i) const {
        return min_price_ + static_cast<std::uint32_t>(i) * tick_size_;
    }

    bool reduce(std::uint64_t ref, std::uint32_t shares) {
        const auto it = orders_.find(ref);
        if (it == orders_.end()) return false;
        Order& o = it->second;
        // Clamp: a feed gap can deliver an execute larger than what we hold;
        // draining to zero keeps the aggregates consistent either way.
        const std::uint32_t qty = shares < o.shares ? shares : o.shares;
        o.shares -= qty;
        const bool gone = (o.shares == 0);
        const itch::Side side = o.side;
        const std::uint32_t price = o.price;
        if (gone) orders_.erase(it);
        level_sub(side, price, qty, gone);
        return true;
    }

    void level_sub(itch::Side side, std::uint32_t price, std::uint32_t shares, bool order_gone) {
        const std::size_t i = idx(price);  // price came off a resting order, already in range
        if (side == itch::Side::Buy) {
            Level& l = bids_[i];
            l.shares -= (shares < l.shares ? shares : l.shares);
            if (order_gone && l.orders > 0) --l.orders;
            // Empty levels must vanish immediately: a phantom level would
            // corrupt best_bid/best_ask and depth counts (regression-tested).
            if (l.orders == 0) {
                --bid_level_count_;
                if (best_bid_idx_ && *best_bid_idx_ == i) retreat_best_bid();
            }
        } else {
            Level& l = asks_[i];
            l.shares -= (shares < l.shares ? shares : l.shares);
            if (order_gone && l.orders > 0) --l.orders;
            if (l.orders == 0) {
                --ask_level_count_;
                if (best_ask_idx_ && *best_ask_idx_ == i) advance_best_ask();
            }
        }
    }

    // The emptied index was the best bid: the next best is the highest
    // occupied index below it, so walk down until one is found or the
    // ladder floor is reached.
    void retreat_best_bid() {
        for (std::size_t i = *best_bid_idx_; i-- > 0;) {
            if (bids_[i].orders > 0) {
                best_bid_idx_ = i;
                return;
            }
        }
        best_bid_idx_.reset();
    }

    // Mirror of retreat_best_bid() for the ask side: the next best ask is
    // the lowest occupied index above the one that just emptied out.
    void advance_best_ask() {
        for (std::size_t i = *best_ask_idx_ + 1; i < num_ticks_; ++i) {
            if (asks_[i].orders > 0) {
                best_ask_idx_ = i;
                return;
            }
        }
        best_ask_idx_.reset();
    }

    std::uint32_t tick_size_;
    std::uint32_t min_price_;
    std::uint32_t max_price_;
    std::size_t num_ticks_;
    std::vector<Level> bids_;
    std::vector<Level> asks_;
    std::size_t bid_level_count_ = 0;
    std::size_t ask_level_count_ = 0;
    std::optional<std::size_t> best_bid_idx_;
    std::optional<std::size_t> best_ask_idx_;
    std::unordered_map<std::uint64_t, Order> orders_;  // the locator, shared shape with OrderBook
};

}  // namespace book
