#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "itch/messages.hpp"

namespace book {

// Per-order state the books need to resolve an ITCH execute/cancel/delete/
// replace, which reference the order id only — never the price or side.
struct RestingOrder {
    std::uint32_t shares;
    std::uint32_t price;
    itch::Side side;
};

// Order-reference locator: an open-addressing hash table (linear probing,
// backward-shift deletion, no tombstones) keyed by the 64-bit ITCH order
// reference number.
//
// Why not std::unordered_map: it allocates a node on every insert and frees
// it on every erase, so every Add/Delete/Replace on the hot path goes
// through the allocator. Here all slots live in one contiguous vector sized
// up front (`reserve()` / the constructor's `expected_orders`), so insert
// and erase never allocate while the table stays within that reservation.
//
// Why not a dense vector indexed directly by order ref: ITCH order refs are
// unique across the whole day and shared by every symbol, while each book
// here covers one symbol. A per-book array indexed by ref would have to span
// the day's full ref range (hundreds of millions on a real day) for every
// one of the ~8,800 books. Hashing a book's own refs into a table sized for
// that book's open orders keeps memory proportional to what is resting.
//
// ITCH semantics this relies on:
//   - 'A'/'F' insert a new ref; a duplicate ref is rejected (insert()
//     returns false), never double-counted.
//   - 'E'/'C'/'X' look up and shrink an order in place (find()).
//   - 'D' and a fully executed/cancelled order erase the ref.
//   - 'U' erases the original ref and inserts the new one.
//
// Growth: if the number of open orders pushes the load factor past 1/2, the
// table doubles and rehashes. That is an allocation plus an O(n) rehash on
// the message that triggers it, so size `expected_orders` for the expected
// peak; growths() counts how often it happened so a run can check it stayed
// at zero.
class OrderLocator {
public:
    explicit OrderLocator(std::size_t expected_orders = 256) { reserve(expected_orders); }

    // Pre-size for `n` simultaneously open orders without crossing the 1/2
    // load factor. Never shrinks. Intended for setup, not the hot path.
    void reserve(std::size_t n) {
        std::size_t cap = kMinCapacity;
        while (cap < n * 2) cap <<= 1;
        if (cap > slots_.size()) rehash(cap);
    }

    RestingOrder* find(std::uint64_t ref) {
        const std::size_t i = find_index(ref);
        return i == kNotFound ? nullptr : &slots_[i].order;
    }
    const RestingOrder* find(std::uint64_t ref) const {
        const std::size_t i = find_index(ref);
        return i == kNotFound ? nullptr : &slots_[i].order;
    }

    // Returns false (and changes nothing) if `ref` is already present.
    bool insert(std::uint64_t ref, const RestingOrder& order) {
        if ((size_ + 1) * 2 > slots_.size()) {
            rehash(slots_.size() * 2);
            ++growths_;
        }
        std::size_t i = home(ref);
        while (slots_[i].used) {
            if (slots_[i].ref == ref) return false;
            i = (i + 1) & mask_;
        }
        slots_[i].used = true;
        slots_[i].ref = ref;
        slots_[i].order = order;
        ++size_;
        return true;
    }

    // Removes `ref` and copies its state into `out`, with a single probe.
    // Returns false if `ref` is not present.
    bool extract(std::uint64_t ref, RestingOrder& out) {
        const std::size_t i = find_index(ref);
        if (i == kNotFound) return false;
        out = slots_[i].order;
        erase_at(i);
        return true;
    }

    // Returns false if `ref` is not present.
    bool erase(std::uint64_t ref) {
        const std::size_t i = find_index(ref);
        if (i == kNotFound) return false;
        erase_at(i);
        return true;
    }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return slots_.size(); }
    std::size_t growths() const { return growths_; }

private:
    struct Slot {
        std::uint64_t ref = 0;
        RestingOrder order{};
        bool used = false;
    };

    static constexpr std::size_t kMinCapacity = 16;
    static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

    // Fibonacci hashing: order refs arrive close to sequential, which would
    // cluster badly under a plain mask. Multiplying by 2^64/phi spreads
    // consecutive refs across the table; the top bits are the best mixed.
    std::size_t home(std::uint64_t ref) const {
        return static_cast<std::size_t>((ref * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    std::size_t find_index(std::uint64_t ref) const {
        std::size_t i = home(ref);
        while (slots_[i].used) {
            if (slots_[i].ref == ref) return i;
            i = (i + 1) & mask_;
        }
        return kNotFound;
    }

    // Backward-shift deletion: pull later members of the probe run back
    // into the hole so lookups never need tombstones and the table never
    // degrades under the add/delete churn of a trading day.
    void erase_at(std::size_t hole) {
        std::size_t j = hole;
        for (;;) {
            j = (j + 1) & mask_;
            if (!slots_[j].used) break;
            const std::size_t h = home(slots_[j].ref);
            // Slot j may move into `hole` only if its home position is not
            // cyclically inside (hole, j].
            const bool home_in_range =
                hole <= j ? (hole < h && h <= j) : (hole < h || h <= j);
            if (home_in_range) continue;
            slots_[hole] = slots_[j];
            hole = j;
        }
        slots_[hole].used = false;
        --size_;
    }

    void rehash(std::size_t new_cap) {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(new_cap, Slot{});
        mask_ = new_cap - 1;
        unsigned bits = 0;
        while ((std::size_t{1} << bits) < new_cap) ++bits;
        shift_ = 64 - bits;
        size_ = 0;
        for (const Slot& s : old) {
            if (!s.used) continue;
            std::size_t i = home(s.ref);
            while (slots_[i].used) i = (i + 1) & mask_;
            slots_[i] = s;
            ++size_;
        }
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 64;
    std::size_t size_ = 0;
    std::size_t growths_ = 0;
};

}  // namespace book
