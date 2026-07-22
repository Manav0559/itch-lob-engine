#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "pipeline/book_table.hpp"

// The concurrency boundary between the ingest side (whatever thread or
// threads actually mutate a pipeline::BookTable — replay_main's single
// thread, or replay_threaded's book-builder thread) and a read-only query
// surface (net::QueryServer) that wants to answer "what's the best bid/ask,
// depth, and open-order count for this symbol *right now*" without either
// touching the book-mutation hot path or reading a book concurrently with a
// writer.
//
// The design deliberately avoids two tempting-but-wrong shortcuts:
//   - Handing the query thread a reference to the live BookTable and letting
//     it call best_bid()/best_ask() directly: BookType (LadderBook/OrderBook)
//     has no internal synchronization — that's a real data race the moment a
//     query runs concurrently with an add/execute/cancel/delete/replace.
//   - Locking around every message on the ingest side so the live table is
//     always safe to read: that puts a mutex on the single hottest call path
//     in the whole engine (see book_table.hpp's header comment) for the sake
//     of a feature that only ever needs an eventually-consistent view.
//
// Instead, the ingest side periodically (every N messages, N in the
// thousands — see src/replay_query_main.cpp) takes a read-only walk over its
// own BookTable — safe, because on the ingest side nothing else is mutating
// it concurrently — copies out the handful of fields a query actually needs
// into a plain-value snapshot, and publishes that snapshot into a
// SnapshotStore. The mutex below is only ever held for that final vector
// swap (microseconds) and for a query thread's copy-out (also microseconds,
// bounded by symbol count, not book depth) — never for however long a
// message decode or a book mutation takes, and never for the O(book size)
// walk that builds the snapshot itself, which happens before the lock is
// taken.
namespace pipeline {

// Plain-value copy of the handful of fields a query answers with — no
// pointers/references into the live book, no BookType dependency, so this
// struct is safe to hand across the mutex above and safe to hand across a
// socket after that (see net::query_server.hpp).
struct LocateSnapshot {
    std::uint16_t locate = 0;

    bool has_bid = false;
    std::uint32_t bid_price = 0;
    std::uint64_t bid_shares = 0;

    bool has_ask = false;
    std::uint32_t ask_price = 0;
    std::uint64_t ask_shares = 0;

    std::size_t open_orders = 0;
    std::size_t bid_levels = 0;
    std::size_t ask_levels = 0;
};

// Read-only walk over a BookTable, called from the ingest side only (see the
// namespace-level comment for why that matters). O(number of locates seen so
// far), not O(number of resting orders) — for_each visits one entry per
// constructed book, not per price level or per order.
template <typename BookType>
std::vector<LocateSnapshot> snapshot_book_table(const BookTable<BookType>& books) {
    std::vector<LocateSnapshot> out;
    books.for_each([&](std::uint16_t locate, const BookType& b) {
        LocateSnapshot s;
        s.locate = locate;
        if (const auto q = b.best_bid()) {
            s.has_bid = true;
            s.bid_price = q->price;
            s.bid_shares = q->shares;
        }
        if (const auto q = b.best_ask()) {
            s.has_ask = true;
            s.ask_price = q->price;
            s.ask_shares = q->shares;
        }
        s.open_orders = b.open_orders();
        s.bid_levels = b.bid_levels();
        s.ask_levels = b.ask_levels();
        out.push_back(s);
    });
    return out;
}

// The mutex-guarded handoff point itself. One writer (the ingest side,
// calling publish() at a coarse cadence of its own choosing) and any number
// of readers (query-server connection threads, calling read_all()/find()
// per request) — a plain std::mutex is enough here: contention is bounded by
// how often publish() runs, which is deliberately rare.
class SnapshotStore {
public:
    // Swaps in a freshly built snapshot, plus the ingest side's running
    // unknown_refs count (pipeline::BookBuilder::unknown_refs — see
    // dispatch_to_book.hpp) as of the same instant, under the same lock —
    // so a query answer can always be read alongside how out-of-sync the
    // book it came from is, instead of that count being visible only in the
    // ingest process's own stdout report at the very end of a run. See
    // src/replay_query_main.cpp's report() and net::query_wire's *_json
    // functions for where this actually reaches a caller.
    void publish(std::vector<LocateSnapshot> next, std::uint64_t unknown_refs = 0) {
        std::lock_guard<std::mutex> lock(mu_);
        data_ = std::move(next);
        unknown_refs_ = unknown_refs;
        ++version_;
    }

    // Full copy of the most recently published snapshot — never the live
    // table, so this is safe to call from any thread at any time, including
    // concurrently with publish().
    std::vector<LocateSnapshot> read_all() const {
        std::lock_guard<std::mutex> lock(mu_);
        return data_;
    }

    std::uint64_t unknown_refs() const {
        std::lock_guard<std::mutex> lock(mu_);
        return unknown_refs_;
    }

    std::optional<LocateSnapshot> find(std::uint16_t locate) const {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& s : data_)
            if (s.locate == locate) return s;
        return std::nullopt;
    }

    // Monotonically increasing count of publish() calls — lets a query
    // response report which generation of the book it reflects, so a caller
    // can tell "no data yet" (0) apart from "data as of snapshot #N" and
    // notice when two responses came from the same, unchanged snapshot.
    std::uint64_t version() const {
        std::lock_guard<std::mutex> lock(mu_);
        return version_;
    }

private:
    mutable std::mutex mu_;
    std::vector<LocateSnapshot> data_;
    std::uint64_t version_ = 0;
    std::uint64_t unknown_refs_ = 0;
};

}  // namespace pipeline
