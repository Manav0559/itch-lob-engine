#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "book/ladder_book.hpp"
#include "book/order_book.hpp"
#include "itch/messages.hpp"
#include "pipeline/book_table.hpp"
#include "pipeline/message.hpp"

namespace pipeline {

// book::LadderBook needs a price window at construction, so it can't be
// default-constructed the way book::OrderBook can — this specializes how
// BookTable builds a LadderBook on first sight of a locate: the 'A' that
// introduces a new symbol carries that symbol's first known price, which
// becomes the ladder's center. kTickSize=100 and kWindowPct=0.30 are not
// arbitrary: they're exactly what bench/bench_main.cpp's BookStore<LadderBook>
// (now superseded by this shared table) actually measured LadderBook's
// advantage under (see README's benchmark table). kTickSize=100 is a real
// one-cent US-equity minimum tick in ITCH's 1/10000-dollar price units — using
// tick_size=1 here instead would silently allocate a ladder ~100x larger than
// the one the benchmark numbers describe, for the same price window. The
// production default and the benchmarked configuration must be the same
// configuration, not two that happen to share a class name.
template <>
struct BookTraits<book::LadderBook> {
    static constexpr std::uint32_t kFallbackBase = 500'000;  // $50.00: unseen-locate fallback
    static constexpr std::uint32_t kTickSize = 100;          // $0.01, in ITCH's 1/10000-dollar units
    static constexpr double kWindowPct = 0.30;
    // $10,000/share, in ITCH's 1/10000-dollar units — well above any real
    // US equity, generous headroom for a halted/gapped name. price_hint is
    // the first 'A' price seen for a locate, read straight off the wire
    // with no upstream range check (see itch::dispatch) — a corrupt or
    // adversarial frame near UINT32_MAX would otherwise size this ladder's
    // window (kWindowPct wider on each side) into a multi-hundred-million-
    // tick allocation attempt per locate. Falling back to kFallbackBase for
    // anything outside this bound keeps construction cost bounded and
    // predictable regardless of what the wire hands us.
    static constexpr std::uint32_t kMaxSanePrice = 100'000'000;
    static book::LadderBook make(std::uint32_t price_hint) {
        const std::uint32_t base =
            (price_hint != 0 && price_hint <= kMaxSanePrice) ? price_hint : kFallbackBase;
        return book::LadderBook(base, kTickSize, kWindowPct);
    }
};

// Routes decoded events into one book per stock locate — via a dense,
// locate-indexed BookTable rather than a hash map (see
// pipeline/book_table.hpp for why) — and keeps per-type counts. Unknown
// order refs are counted, not fatal: on a partial replay (or a corrupt file)
// they tell you how out-of-sync the book is.
//
// BookType defaults to book::LadderBook: the flat tick-ladder book measured
// faster than book::OrderBook (std::map ladders) across every message type,
// with the gap widening sharply in the tail (see README's benchmark table
// and docs/devlog-orderbook-vs-ladderbook.md). Pass BookType=book::OrderBook
// explicitly (replay/replay_threaded's --map flag) to run the slower,
// unbounded-price-range baseline instead — kept as an explicit A/B option,
// the same way --legacy keeps the whole-file-into-memory I/O path around.
//
// Shared by both replay binaries: replay_main.cpp drives it straight from
// itch::dispatch on raw bytes (single-threaded), replay_threaded_main.cpp
// drives it from Envelopes handed across the SPSC queue by a separate
// parser thread (via dispatch_to_book below). Either way, book-building
// bottoms out in these same on_* methods — there is exactly one
// implementation of "decode this message into this book," not one per
// binary.
template <typename BookType = book::LadderBook>
struct BookBuilder {
    BookTable<BookType> books;
    std::array<std::uint64_t, 256> counts{};
    std::uint64_t unknown_refs = 0;

    void bump(char t) { ++counts[static_cast<unsigned char>(t)]; }

    void on_add(const itch::AddOrder& m) {
        bump('A');
        if (!books.get_or_create(m.hdr.locate, m.price).add(m.ref, m.side, m.shares, m.price))
            ++unknown_refs;
    }
    void on_execute(const itch::OrderExecuted& m) {
        bump('E');
        if (!books.get_or_create(m.hdr.locate, 0).execute(m.ref, m.shares)) ++unknown_refs;
    }
    void on_execute_price(const itch::OrderExecutedPrice& m) {
        bump('C');
        if (!books.get_or_create(m.hdr.locate, 0).execute(m.ref, m.shares)) ++unknown_refs;
    }
    void on_cancel(const itch::OrderCancel& m) {
        bump('X');
        if (!books.get_or_create(m.hdr.locate, 0).cancel(m.ref, m.canceled)) ++unknown_refs;
    }
    void on_delete(const itch::OrderDelete& m) {
        bump('D');
        if (!books.get_or_create(m.hdr.locate, 0).remove(m.ref)) ++unknown_refs;
    }
    void on_replace(const itch::OrderReplace& m) {
        bump('U');
        if (!books.get_or_create(m.hdr.locate, m.price)
                 .replace(m.orig_ref, m.new_ref, m.shares, m.price))
            ++unknown_refs;
    }
    void on_other(char type, std::size_t) { bump(type); }
};

// Re-dispatches one Envelope (already decoded by the parser thread) into a
// BookBuilder — the Envelope-side mirror of itch::dispatch's raw-byte
// switch in parser.hpp. Both entry points bottom out in the same
// BookBuilder::on_* calls, so the "apply this message to the book" logic
// itself is never duplicated, only its entry point (raw bytes vs. an
// already-decoded envelope crossing a thread boundary).
template <typename BookType>
inline void dispatch_to_book(const Envelope& env, BookBuilder<BookType>& h) {
    switch (env.type) {
        case 'A':
        case 'F':
            h.on_add(env.add);
            break;
        case 'E':
            h.on_execute(env.exec);
            break;
        case 'C':
            h.on_execute_price(env.exec_price);
            break;
        case 'X':
            h.on_cancel(env.cancel);
            break;
        case 'D':
            h.on_delete(env.del);
            break;
        case 'U':
            h.on_replace(env.replace);
            break;
        default:
            h.on_other(env.type, 0);
            break;
    }
}

}  // namespace pipeline
