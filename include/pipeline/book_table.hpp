#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Dense, locate-indexed replacement for std::unordered_map<uint16_t, BookType>.
//
// ITCH stock-locate codes are small, session-dense integers (1..N, N in the
// low thousands even for the full Nasdaq universe) assigned once at the
// start of the trading day — exactly the case where a flat, directly-indexed
// array beats a hash map on every access: no hash, no bucket walk, no
// rehash-on-insert, one predictable array index plus a presence check. This
// is the single hottest call site in the whole pipeline: every message, of
// every type, resolves its book here before anything else happens to it.
//
// Not every BookType is default-constructible (book::LadderBook needs a
// price window at construction time) — Slot models "constructed" vs "not yet
// seen" with std::optional, and the table is generic over how a book gets
// built via the BookTraits<T> hook below, so book::OrderBook and
// book::LadderBook can share this one table without either type changing.
namespace pipeline {

// Primary template: correct for any BookType that is default-constructible
// (book::OrderBook). Specialize this for a type that needs a construction
// argument — see pipeline/dispatch_to_book.hpp's BookTraits<book::LadderBook>.
template <typename BookType>
struct BookTraits {
    static BookType make(std::uint32_t /*price_hint*/) { return BookType{}; }
};

template <typename BookType>
class BookTable {
public:
    explicit BookTable(std::size_t initial_capacity = 4096) { slots_.reserve(initial_capacity); }

    // Returns the book for `locate`, constructing it on first sight via
    // BookTraits<BookType>::make(price_hint). price_hint is consulted only
    // the first time a given locate is seen — a book, once built, keeps its
    // original construction regardless of what later hints come in for it.
    // Amortized O(1): the vector resize on a brand-new, higher locate is rare
    // (once per newly-seen symbol, not per message) and never shrinks.
    BookType& get_or_create(std::uint16_t locate, std::uint32_t price_hint) {
        const auto idx = static_cast<std::size_t>(locate);
        if (idx >= slots_.size()) slots_.resize(idx + 1);
        auto& slot = slots_[idx];
        if (!slot) slot.emplace(BookTraits<BookType>::make(price_hint));
        return *slot;
    }

    // Read-only lookup for callers that must not create a book as a side
    // effect (reporting/metrics code walking books that may not exist yet).
    const BookType* find(std::uint16_t locate) const {
        const auto idx = static_cast<std::size_t>(locate);
        if (idx >= slots_.size()) return nullptr;
        return slots_[idx] ? &*slots_[idx] : nullptr;
    }

    // Reporting/rollup support (replaces range-for over a
    // std::unordered_map) — visits only locates that have actually been
    // constructed, in locate order.
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if (slots_[i]) fn(static_cast<std::uint16_t>(i), *slots_[i]);
    }

    std::size_t book_count() const {
        std::size_t n = 0;
        for (const auto& s : slots_) n += s.has_value();
        return n;
    }

private:
    std::vector<std::optional<BookType>> slots_;
};

}  // namespace pipeline
