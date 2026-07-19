#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "book/order_book.hpp"
#include "itch/messages.hpp"
#include "pipeline/message.hpp"

namespace pipeline {

// Routes decoded events into one OrderBook per stock locate and keeps
// per-type counts. Unknown order refs are counted, not fatal: on a partial
// replay (or a corrupt file) they tell you how out-of-sync the book is.
//
// Shared by both replay binaries: replay_main.cpp drives it straight from
// itch::dispatch on raw bytes (single-threaded), replay_threaded_main.cpp
// drives it from Envelopes handed across the SPSC queue by a separate
// parser thread (via dispatch_to_book below). Either way, book-building
// bottoms out in these same on_* methods — there is exactly one
// implementation of "decode this message into this book," not one per
// binary.
struct BookBuilder {
    std::unordered_map<std::uint16_t, book::OrderBook> books;
    std::array<std::uint64_t, 256> counts{};
    std::uint64_t unknown_refs = 0;

    void bump(char t) { ++counts[static_cast<unsigned char>(t)]; }

    void on_add(const itch::AddOrder& m) {
        bump('A');
        if (!books[m.hdr.locate].add(m.ref, m.side, m.shares, m.price)) ++unknown_refs;
    }
    void on_execute(const itch::OrderExecuted& m) {
        bump('E');
        if (!books[m.hdr.locate].execute(m.ref, m.shares)) ++unknown_refs;
    }
    void on_execute_price(const itch::OrderExecutedPrice& m) {
        bump('C');
        if (!books[m.hdr.locate].execute(m.ref, m.shares)) ++unknown_refs;
    }
    void on_cancel(const itch::OrderCancel& m) {
        bump('X');
        if (!books[m.hdr.locate].cancel(m.ref, m.canceled)) ++unknown_refs;
    }
    void on_delete(const itch::OrderDelete& m) {
        bump('D');
        if (!books[m.hdr.locate].remove(m.ref)) ++unknown_refs;
    }
    void on_replace(const itch::OrderReplace& m) {
        bump('U');
        if (!books[m.hdr.locate].replace(m.orig_ref, m.new_ref, m.shares, m.price))
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
inline void dispatch_to_book(const Envelope& env, BookBuilder& h) {
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
