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
// the cost of a bounded price range: add() on a price outside the current
// range first tries to grow the ladder to fit (see grow_to_include() below)
// and only rejects — the same way it rejects a duplicate ref, a clean false,
// no partial state — once that range would exceed a hard allocation ceiling.
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
// gaps or halts past that band no longer gets every message past it silently
// rejected: add() detects an out-of-range price and rebuilds the ladder
// around a wider window (grow_to_include(), below) before retrying, up to a
// hard tick-count ceiling (kMaxTicks) that keeps a single adversarial or
// corrupt wire price from turning into an unbounded allocation. This
// replaces a real, measured production bug — see
// docs/devlog-orderbook-vs-ladderbook.md's "Closing the open question": a
// fixed, never-widening window silently dropped ~30% of order-mutating
// messages on a full real NASDAQ day file, because roughly a third of real
// symbols moved outside a flat +/-30% band at some point in the session.
// This bounded-but-growable range, weighed against std::map's unbounded
// range, is exactly what the benchmark harness measures OrderBook against.
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
                     window_high(base_price, window_pct), window_pct) {}

    bool add(std::uint64_t ref, itch::Side side, std::uint32_t shares, std::uint32_t price) {
        // An out-of-range price grows the ladder (see grow_to_include())
        // instead of getting rejected outright — growth can itself refuse
        // (kMaxTicks) for a price so extreme it would blow the allocation
        // ceiling, which still falls through to the same false-return path
        // below. idx() below is integer division by tick_size_ — a price
        // that isn't grid-aligned to min_price_ would floor-divide into
        // whatever slot is nearest, silently colliding with a real adjacent
        // price level instead of getting its own. That's harmless for a
        // benchmark seeded with grid-aligned synthetic prices (this ladder's
        // original use), but a real ITCH feed can carry sub-penny prices
        // (Reg NMS Rule 612 permits them under $1), so this check matters
        // now that LadderBook is wired into production against real files.
        if (!in_range(price) && !grow_to_include(price)) return false;
        if ((price - min_price_) % tick_size_ != 0) return false;
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

    LadderBook(std::uint32_t tick_size, std::uint32_t min_price, std::uint32_t max_price,
               double window_pct)
        : tick_size_(tick_size),
          min_price_(min_price),
          max_price_(check_range(min_price, max_price)),
          window_pct_(window_pct),
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

    // Hard ceiling on ticks-per-side any single grow_to_include() call (or
    // the initial construction) will ever allocate for, independent of
    // tick_size_ — this is a property of the class itself, not something
    // every caller has to remember to clamp the price it grows toward (see
    // pipeline::BookTraits<LadderBook>::kMaxSanePrice in dispatch_to_book.hpp
    // for the analogous, but construction-only and caller-side, guard this
    // complements). At production's tick_size_=100, 8,000,000 ticks per side
    // covers an $8,000,000 price span — already absurd for a real equity —
    // while still bounding a single LadderBook's worst-case footprint to a
    // few hundred MB (2 sides * 8e6 ticks * sizeof(Level)) instead of
    // whatever an adversarial or corrupt wire price would otherwise demand.
    static constexpr std::size_t kMaxTicks = 8'000'000;

    // Called by add() when `price` falls outside [min_price_, max_price_]:
    // computes a wider range (same window_pct_ this book was built with,
    // re-centered so it still comfortably covers `price`) and rebuilds the
    // ladder around it, re-homing every resting order's level at its new
    // array index. Returns false — refusing to grow, leaving add() to reject
    // the price exactly as it did before this existed — only when the
    // resulting range would exceed kMaxTicks; true otherwise, including the
    // case where growth succeeds but the grid-alignment check right after
    // this call still rejects the price on its own separate grounds.
    bool grow_to_include(std::uint32_t price) {
        std::uint32_t new_min = min_price_;
        std::uint32_t new_max = max_price_;
        if (price < min_price_)
            new_min = std::min(min_price_, snap_to_tick(window_low(price, window_pct_), tick_size_));
        if (price > max_price_) new_max = std::max(max_price_, window_high(price, window_pct_));
        if (new_min == min_price_ && new_max == max_price_) return true;  // already covers it

        const std::size_t new_num_ticks =
            static_cast<std::size_t>(new_max - new_min) / tick_size_ + 1;
        if (new_num_ticks > kMaxTicks) return false;

        resize_range(new_min, new_max, new_num_ticks);
        return true;
    }

    // Reallocates bids_/asks_ to the new range and copies every existing
    // level to its new index (offset by how far the low end moved) — never
    // drops a resting order or level, since min_price_ only ever moves down
    // and max_price_ only ever moves up (grow_to_include() never shrinks the
    // range), so every old index has a valid new home.
    void resize_range(std::uint32_t new_min, std::uint32_t new_max, std::size_t new_num_ticks) {
        const std::size_t offset = static_cast<std::size_t>(min_price_ - new_min) / tick_size_;

        std::vector<Level> new_bids(new_num_ticks);
        std::vector<Level> new_asks(new_num_ticks);
        for (std::size_t i = 0; i < num_ticks_; ++i) {
            new_bids[i + offset] = bids_[i];
            new_asks[i + offset] = asks_[i];
        }
        bids_ = std::move(new_bids);
        asks_ = std::move(new_asks);

        min_price_ = new_min;
        max_price_ = new_max;
        num_ticks_ = new_num_ticks;
        if (best_bid_idx_) best_bid_idx_ = *best_bid_idx_ + offset;
        if (best_ask_idx_) best_ask_idx_ = *best_ask_idx_ + offset;
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
    double window_pct_;
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
