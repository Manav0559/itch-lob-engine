#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>

#include "itch/messages.hpp"

namespace book {

struct Level {
    std::uint64_t shares = 0;
    std::uint32_t orders = 0;
};

struct Quote {
    std::uint32_t price = 0;
    std::uint64_t shares = 0;
};

// Price-level book driven by ITCH order-id events. Executes, cancels and
// deletes reference the order id — never the price — so per-order state
// lives in a hash-map locator and the price ladders carry only per-level
// aggregates. Mutators return false on unknown/duplicate refs instead of
// throwing: on a real feed those indicate a gap, and the caller decides
// whether to count or resync.
//
// std::map ladders are the deliberate v1 baseline. The flat tick-ladder
// variant ships together with the benchmark harness so map-vs-ladder is a
// measured comparison, not an asserted one.
class OrderBook {
public:
    bool add(std::uint64_t ref, itch::Side side, std::uint32_t shares,
             std::uint32_t price) {
        const auto [it, inserted] = orders_.try_emplace(ref, Order{shares, price, side});
        if (!inserted) return false;  // duplicate ref: never double-count a level
        Level& lvl = (side == itch::Side::Buy) ? bids_[price] : asks_[price];
        lvl.shares += shares;
        lvl.orders += 1;
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
        if (bids_.empty()) return std::nullopt;
        const auto& [price, lvl] = *bids_.begin();
        return Quote{price, lvl.shares};
    }
    std::optional<Quote> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        const auto& [price, lvl] = *asks_.begin();
        return Quote{price, lvl.shares};
    }

    std::size_t open_orders() const { return orders_.size(); }
    std::size_t bid_levels() const { return bids_.size(); }
    std::size_t ask_levels() const { return asks_.size(); }

private:
    struct Order {
        std::uint32_t shares;
        std::uint32_t price;
        itch::Side side;
    };

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

    template <typename Map>
    static void sub_in(Map& m, std::uint32_t price, std::uint32_t shares, bool order_gone) {
        const auto it = m.find(price);
        if (it == m.end()) return;
        Level& l = it->second;
        l.shares -= (shares < l.shares ? shares : l.shares);
        if (order_gone && l.orders > 0) --l.orders;
        // Empty levels must vanish immediately: a phantom level would corrupt
        // best_bid/best_ask and depth counts (regression-tested).
        if (l.orders == 0) m.erase(it);
    }

    void level_sub(itch::Side side, std::uint32_t price, std::uint32_t shares,
                   bool order_gone) {
        if (side == itch::Side::Buy) {
            sub_in(bids_, price, shares, order_gone);
        } else {
            sub_in(asks_, price, shares, order_gone);
        }
    }

    std::map<std::uint32_t, Level, std::greater<>> bids_;  // best bid first
    std::map<std::uint32_t, Level> asks_;                  // best ask first
    std::unordered_map<std::uint64_t, Order> orders_;      // the locator
};

}  // namespace book
