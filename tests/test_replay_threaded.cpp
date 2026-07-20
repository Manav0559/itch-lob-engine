#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "book/order_book.hpp"
#include "itch/encode.hpp"
#include "itch/parser.hpp"
#include "pipeline/dispatch_to_book.hpp"
#include "pipeline/message.hpp"
#include "pipeline/spsc_queue.hpp"

// Verifies that decoupling parse-from-book-mutation into two threads (the
// change replay_threaded_main.cpp makes over replay_main.cpp) does not
// change what the book ends up looking like — only how it gets there. Both
// paths below are driven by the exact same BookBuilder::on_* methods
// (shared via pipeline/dispatch_to_book.hpp), so this is really a check that
// the threading/queue plumbing routes every message through exactly once,
// in order, per stock locate.
namespace {

using pipeline::BookBuilder;
using pipeline::Envelope;

// Same synthetic session as replay_main.cpp / replay_threaded_main.cpp's
// --selftest, kept in sync deliberately: it exercises adds, an execute, a
// partial cancel, a replace and a delete across two symbols/locates, plus
// one undecoded type ('S') that must be skipped without desyncing the
// stream.
std::vector<std::uint8_t> synthetic_session() {
    using namespace itch::encode;
    Msg system_event;
    header(system_event, 'S', 0, 1);
    system_event.push_back('O');

    return stream({
        system_event,
        add_order(1, 100, 1001, itch::Side::Buy, 300, "AAPL", 1'500'000),
        add_order(1, 110, 1002, itch::Side::Buy, 200, "AAPL", 1'499'900),
        add_order(1, 120, 2001, itch::Side::Sell, 500, "AAPL", 1'500'100),
        add_order(2, 130, 3001, itch::Side::Buy, 1000, "MSFT", 4'200'000),
        executed(1, 140, 1001, 300, 90001),
        cancel(1, 150, 2001, 100),
        replace(2, 160, 3001, 3002, 800, 4'199'500),
        del(1, 170, 1002),
    });
}

// A deliberately small capacity: with ~9 frames flowing through a
// 4-capacity queue, push() is guaranteed to hit "full" and spin-retry
// several times over the run, so this test exercises the same backpressure
// path replay_threaded_main.cpp relies on, not just the uncontended case.
constexpr std::size_t kTestQueueCapacity = 4;
using TestQueue = pipeline::SpscQueue<Envelope, kTestQueueCapacity>;

// Minimal stand-in for replay_threaded_main.cpp's QueueProducer: same
// itch::dispatch Handler interface, same envelope tagging convention, just
// without the max-occupancy bookkeeping this test doesn't need.
struct TestProducer {
    TestQueue& queue;

    void push_spin(const Envelope& env) {
        while (!queue.push(env)) std::this_thread::yield();
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
    // See include/pipeline/threaded_replay.hpp's on_other for why `.add = {}`
    // is spelled out explicitly (GCC-only -Wmissing-field-initializers).
    void on_other(char type, std::size_t) { push_spin(Envelope{.type = type, .add = {}}); }
};

// Replays `data` through a real parser thread + book-builder thread joined
// by TestQueue, mirroring replay_threaded_main.cpp's run_pipeline/consume
// shape at test scale.
BookBuilder replay_threaded(const std::uint8_t* data, std::size_t len) {
    TestQueue queue;
    std::atomic<bool> producer_done{false};
    TestProducer producer{queue};
    BookBuilder builder;

    std::thread book_builder_thread([&] {
        Envelope env;
        while (true) {
            if (queue.pop(env)) {
                pipeline::dispatch_to_book(env, builder);
                continue;
            }
            if (producer_done.load(std::memory_order_acquire)) {
                while (queue.pop(env)) pipeline::dispatch_to_book(env, builder);
                break;
            }
            std::this_thread::yield();
        }
    });
    std::thread parser_thread([&] {
        itch::parse_stream(data, len, producer);
        producer_done.store(true, std::memory_order_release);
    });

    parser_thread.join();
    book_builder_thread.join();
    return builder;
}

void check_same_book_state(const BookBuilder& single, const BookBuilder& threaded) {
    REQUIRE(single.books.size() == threaded.books.size());
    for (const auto& [locate, single_book] : single.books) {
        INFO("locate " << locate);
        const auto it = threaded.books.find(locate);
        REQUIRE(it != threaded.books.end());
        const book::OrderBook& threaded_book = it->second;

        CHECK(single_book.open_orders() == threaded_book.open_orders());
        CHECK(single_book.bid_levels() == threaded_book.bid_levels());
        CHECK(single_book.ask_levels() == threaded_book.ask_levels());
        CHECK((single_book.best_bid().has_value()) == (threaded_book.best_bid().has_value()));
        if (single_book.best_bid().has_value() && threaded_book.best_bid().has_value()) {
            CHECK(single_book.best_bid()->price == threaded_book.best_bid()->price);
            CHECK(single_book.best_bid()->shares == threaded_book.best_bid()->shares);
        }
        CHECK((single_book.best_ask().has_value()) == (threaded_book.best_ask().has_value()));
        if (single_book.best_ask().has_value() && threaded_book.best_ask().has_value()) {
            CHECK(single_book.best_ask()->price == threaded_book.best_ask()->price);
            CHECK(single_book.best_ask()->shares == threaded_book.best_ask()->shares);
        }
    }
}

}  // namespace

TEST_CASE("threaded pipeline produces identical book state to the single-threaded path") {
    const std::vector<std::uint8_t> buf = synthetic_session();

    BookBuilder single;
    REQUIRE(itch::parse_stream(buf.data(), buf.size(), single) == 9);

    const BookBuilder threaded = replay_threaded(buf.data(), buf.size());

    check_same_book_state(single, threaded);
    CHECK(single.unknown_refs == threaded.unknown_refs);
}

TEST_CASE("threaded pipeline preserves per-type frame counts") {
    const std::vector<std::uint8_t> buf = synthetic_session();

    BookBuilder single;
    itch::parse_stream(buf.data(), buf.size(), single);
    const BookBuilder threaded = replay_threaded(buf.data(), buf.size());

    for (int t = 0; t < 256; ++t) CHECK(single.counts[static_cast<std::size_t>(t)] ==
                                        threaded.counts[static_cast<std::size_t>(t)]);
}
