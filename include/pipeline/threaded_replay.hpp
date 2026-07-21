#pragma once
#include <atomic>
#include <cstddef>
#include <optional>
#include <thread>
#include <utility>

#include "book/ladder_book.hpp"
#include "pipeline/dispatch_to_book.hpp"
#include "pipeline/message.hpp"
#include "pipeline/spsc_queue.hpp"
#include "pipeline/thread_affinity.hpp"

// Reusable parser-thread/book-builder-thread pipeline machinery, factored
// out of replay_threaded_main.cpp so that bench/bench_threaded_main.cpp can
// drive the exact same two-thread pipeline the standalone binary does,
// rather than carrying a second copy that could quietly drift out of sync
// with it (same reasoning as bench/synthetic_session.hpp for the generator
// it shares with bench_main.cpp).
namespace pipeline {

// Parser-thread handler: matches the itch::dispatch Handler interface
// exactly (same as BookBuilder does for the single-threaded path), but
// instead of mutating a book inline, it packages each decoded message into
// an Envelope and hands it to the book-builder thread over the queue.
template <std::size_t QueueCapacity>
struct QueueProducer {
    using Queue = SpscQueue<Envelope, QueueCapacity>;

    Queue& queue;
    std::size_t max_occupancy = 0;
    std::size_t frames = 0;

    // Deliberate simple backpressure: if the queue is full, the book-builder
    // thread is behind, so spin-yield and retry rather than drop the
    // message or block on a condvar.
    void push_spin(const Envelope& env) {
        while (!queue.push(env)) std::this_thread::yield();
        const std::size_t occ = queue.size();
        if (occ > max_occupancy) max_occupancy = occ;
        ++frames;
    }

    void on_add(const itch::AddOrder& m) { push_spin(Envelope{.type = 'A', .add = m}); }
    void on_execute(const itch::OrderExecuted& m) {
        push_spin(Envelope{.type = 'E', .exec = m});
    }
    void on_execute_price(const itch::OrderExecutedPrice& m) {
        push_spin(Envelope{.type = 'C', .exec_price = m});
    }
    void on_cancel(const itch::OrderCancel& m) { push_spin(Envelope{.type = 'X', .cancel = m}); }
    void on_delete(const itch::OrderDelete& m) { push_spin(Envelope{.type = 'D', .del = m}); }
    void on_replace(const itch::OrderReplace& m) {
        push_spin(Envelope{.type = 'U', .replace = m});
    }
    // Skipped/corrupt frames don't touch any book, but still get pushed
    // (bare tag, no payload) so the consumer's counts stay a complete
    // picture, owned by one thread, rather than splitting bookkeeping
    // across both.
    // GCC's -Wmissing-field-initializers fires on a designated-initializer
    // aggregate that names `.type` but leaves Envelope's anonymous union
    // unlisted (AppleClang doesn't warn on this — caught by the Linux CI
    // runner). Naming `.add = {}` explicitly satisfies it without changing
    // behavior: the union's active member is irrelevant for a skipped/
    // corrupt frame, which carries no payload either way.
    void on_other(char type, std::size_t) { push_spin(Envelope{.type = type, .add = {}}); }
};

template <typename BookType>
struct ConsumerResult {
    BookBuilder<BookType> builder;
    std::size_t frames = 0;
};

// Book-builder thread body: pops envelopes and re-dispatches them into the
// book. producer_done is checked only after a pop comes up empty, and once
// it's observed true the queue is drained completely before exiting — the
// producer can't push anything more after setting it, so a full drain at
// that point is guaranteed to be the true end of the stream.
template <typename Queue, typename BookType>
void consume(Queue& queue, const std::atomic<bool>& producer_done, ConsumerResult<BookType>& result) {
    Envelope env;
    while (true) {
        if (queue.pop(env)) {
            dispatch_to_book(env, result.builder);
            ++result.frames;
            continue;
        }
        if (producer_done.load(std::memory_order_acquire)) {
            while (queue.pop(env)) {
                dispatch_to_book(env, result.builder);
                ++result.frames;
            }
            break;
        }
        std::this_thread::yield();
    }
}

template <typename BookType>
struct RunStats {
    BookBuilder<BookType> builder;
    std::size_t frames = 0;
    std::size_t max_occupancy = 0;
};

// Optional hard-pin request for the two threads run_pipeline spawns. Both
// fields default to unset (no pinning attempted) so every existing call site
// keeps its current behavior unchanged. See pipeline/thread_affinity.hpp for
// what "pinned" actually means per platform (a real bind on Linux, a
// best-effort scheduling hint on macOS).
struct CoreAffinity {
    std::optional<int> parser_core;
    std::optional<int> book_builder_core;
};

// Runs `decode` (a callable that walks the input and feeds a
// QueueProducer<QueueCapacity>) on a dedicated parser thread while a
// book-builder thread drains the queue concurrently, then joins both. Every
// RunStats field is written by exactly one of the two threads and only read
// here after both threads have joined, so no additional synchronization is
// needed beyond the join()s and the producer_done flag consume() already
// uses to know when to stop.
//
// BookType defaults to book::LadderBook (see dispatch_to_book.hpp's
// BookBuilder) so existing call sites (`run_pipeline<kQueueCapacity>(...)`)
// get the faster book automatically; pass it explicitly
// (`run_pipeline<kQueueCapacity, book::OrderBook>(...)`) to A/B against the
// std::map baseline instead.
//
// affinity is opt-in and defaults to {} (no pinning) - existing call sites
// need no changes. Pinning happens from inside each thread's own lambda
// (pin_this_thread_to_core operates on "the calling thread"), before that
// thread does any real work, so the pin request is in effect for the whole
// run, not applied after the fact.
template <std::size_t QueueCapacity, typename BookType = book::LadderBook, typename Decode>
RunStats<BookType> run_pipeline(Decode&& decode, CoreAffinity affinity = {}) {
    using Queue = SpscQueue<Envelope, QueueCapacity>;

    Queue queue;
    std::atomic<bool> producer_done{false};
    QueueProducer<QueueCapacity> producer{queue};
    ConsumerResult<BookType> consumer_result;

    std::thread book_builder_thread([&] {
        if (affinity.book_builder_core) pin_this_thread_to_core(*affinity.book_builder_core);
        consume<Queue, BookType>(queue, producer_done, consumer_result);
    });
    std::thread parser_thread([&] {
        if (affinity.parser_core) pin_this_thread_to_core(*affinity.parser_core);
        decode(producer);
        producer_done.store(true, std::memory_order_release);
    });

    parser_thread.join();
    book_builder_thread.join();

    return RunStats<BookType>{std::move(consumer_result.builder), consumer_result.frames,
                               producer.max_occupancy};
}

}  // namespace pipeline
