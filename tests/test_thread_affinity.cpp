#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "book/order_book.hpp"
#include "itch/encode.hpp"
#include "itch/parser.hpp"
#include "pipeline/thread_affinity.hpp"
#include "pipeline/threaded_replay.hpp"

// Two things worth checking separately: that pin_this_thread_to_core doesn't
// crash/misbehave when called from an arbitrary thread (it's a real syscall/
// Mach call, not just a stored flag), and that requesting affinity through
// run_pipeline's CoreAffinity parameter doesn't change what the pipeline
// computes — pinning is a scheduling hint, not a change to dispatch logic,
// so book state must come out identical to the unpinned path.
namespace {

std::vector<std::uint8_t> synthetic_session() {
    using namespace itch::encode;
    return stream({
        add_order(1, 100, 1001, itch::Side::Buy, 300, "AAPL", 1'500'000),
        add_order(1, 110, 1002, itch::Side::Buy, 200, "AAPL", 1'499'900),
        add_order(1, 120, 2001, itch::Side::Sell, 500, "AAPL", 1'500'100),
        executed(1, 130, 1001, 300, 90001),
        cancel(1, 140, 2001, 100),
        del(1, 150, 1002),
    });
}

}  // namespace

TEST_CASE("pin_this_thread_to_core does not crash on a valid core index", "[concurrency]") {
    // Return value intentionally not asserted true/false: on macOS it's a
    // best-effort hint (see pipeline/thread_affinity.hpp) that can legally
    // fail depending on system state, and on an unsupported platform it's
    // defined to always return false. What must hold everywhere is "calling
    // this does not crash, hang, or corrupt anything" - checked by simply
    // surviving the call.
    (void)pipeline::pin_this_thread_to_core(0);
    SUCCEED("pin_this_thread_to_core(0) returned without crashing");
}

TEST_CASE("run_pipeline with CoreAffinity requested produces identical book state to unpinned",
          "[concurrency]") {
    constexpr std::size_t kCapacity = 8;
    const std::vector<std::uint8_t> buf = synthetic_session();

    const auto unpinned = pipeline::run_pipeline<kCapacity, book::OrderBook>(
        [&](pipeline::QueueProducer<kCapacity>& p) { itch::parse_stream(buf.data(), buf.size(), p); });

    const auto pinned = pipeline::run_pipeline<kCapacity, book::OrderBook>(
        [&](pipeline::QueueProducer<kCapacity>& p) { itch::parse_stream(buf.data(), buf.size(), p); },
        pipeline::CoreAffinity{0, 0});  // both threads request the same core - still must be correct

    REQUIRE(unpinned.frames == pinned.frames);
    REQUIRE(unpinned.builder.books.book_count() == pinned.builder.books.book_count());

    const book::OrderBook* unpinned_book = unpinned.builder.books.find(1);
    const book::OrderBook* pinned_book = pinned.builder.books.find(1);
    REQUIRE(unpinned_book != nullptr);
    REQUIRE(pinned_book != nullptr);

    CHECK(unpinned_book->open_orders() == pinned_book->open_orders());
    CHECK(unpinned_book->bid_levels() == pinned_book->bid_levels());
    CHECK(unpinned_book->ask_levels() == pinned_book->ask_levels());
    REQUIRE(unpinned_book->best_ask().has_value());
    REQUIRE(pinned_book->best_ask().has_value());
    CHECK(unpinned_book->best_ask()->price == pinned_book->best_ask()->price);
    CHECK(unpinned_book->best_ask()->shares == pinned_book->best_ask()->shares);
}
