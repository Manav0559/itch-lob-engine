#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "book/ladder_book.hpp"

using book::LadderBook;
using itch::Side;

// All prices below sit within +/-20% of 10'000'000, LadderBook's default
// window around that reference price — same values test_book.cpp uses
// against OrderBook, so the two suites assert the same behavior.

TEST_CASE("best bid/ask track adds across levels [ladder]") {
    LadderBook b(10'000'000);
    REQUIRE(b.add(1, Side::Buy, 100, 9'990'000));
    REQUIRE(b.add(2, Side::Buy, 200, 10'000'000));
    REQUIRE(b.add(3, Side::Sell, 300, 10'010'000));

    const auto bid = b.best_bid();
    REQUIRE(bid.has_value());
    CHECK(bid->price == 10'000'000);   // highest bid wins
    CHECK(bid->shares == 200);

    const auto ask = b.best_ask();
    REQUIRE(ask.has_value());
    CHECK(ask->price == 10'010'000);   // lowest ask wins
    CHECK(ask->shares == 300);
}

TEST_CASE("partial execute shrinks the level; full execute erases order AND level [ladder]") {
    LadderBook b(10'000'000);
    b.add(1, Side::Buy, 500, 10'000'000);

    REQUIRE(b.execute(1, 200));
    auto bid = b.best_bid();
    REQUIRE(bid.has_value());
    CHECK(bid->shares == 300);
    CHECK(b.open_orders() == 1);

    REQUIRE(b.execute(1, 300));
    CHECK(b.open_orders() == 0);
    CHECK(b.bid_levels() == 0);        // the phantom-level regression check
    CHECK_FALSE(b.best_bid().has_value());
}

TEST_CASE("emptied best level exposes the next level down [ladder]") {
    LadderBook b(10'000'000);
    b.add(1, Side::Buy, 100, 10'000'000);
    b.add(2, Side::Buy, 250, 9'990'000);

    REQUIRE(b.remove(1));
    const auto bid = b.best_bid();
    REQUIRE(bid.has_value());
    CHECK(bid->price == 9'990'000);    // a lingering empty level would mask this
    CHECK(bid->shares == 250);
    CHECK(b.bid_levels() == 1);
}

TEST_CASE("two orders at one price: removing one keeps the level with the remainder [ladder]") {
    LadderBook b(10'000'000);
    b.add(1, Side::Sell, 100, 10'010'000);
    b.add(2, Side::Sell, 400, 10'010'000);

    REQUIRE(b.remove(1));
    const auto ask = b.best_ask();
    REQUIRE(ask.has_value());
    CHECK(ask->shares == 400);
    CHECK(b.ask_levels() == 1);
    CHECK(b.open_orders() == 1);
}

TEST_CASE("partial cancel keeps the order open; delete removes the remainder [ladder]") {
    LadderBook b(10'000'000);
    b.add(1, Side::Buy, 500, 10'000'000);

    REQUIRE(b.cancel(1, 200));
    CHECK(b.open_orders() == 1);
    CHECK(b.best_bid()->shares == 300);

    REQUIRE(b.remove(1));
    CHECK(b.open_orders() == 0);
    CHECK(b.bid_levels() == 0);
}

TEST_CASE("replace moves size and price, keeps side, retires the old ref [ladder]") {
    LadderBook b(10'000'000);
    b.add(1, Side::Buy, 500, 10'000'000);

    REQUIRE(b.replace(1, 2, 300, 9'995'000));
    CHECK(b.bid_levels() == 1);
    const auto bid = b.best_bid();
    REQUIRE(bid.has_value());
    CHECK(bid->price == 9'995'000);
    CHECK(bid->shares == 300);

    CHECK_FALSE(b.execute(1, 100));    // old ref must be gone
    CHECK(b.execute(2, 100));          // new ref is live, same (buy) side
}

TEST_CASE("unknown and duplicate refs are reported, never corrupt aggregates [ladder]") {
    LadderBook b(10'000'000);
    REQUIRE(b.add(1, Side::Buy, 100, 10'000'000));

    CHECK_FALSE(b.add(1, Side::Buy, 999, 10'000'000));  // duplicate add ignored
    CHECK(b.best_bid()->shares == 100);                  // not double-counted

    CHECK_FALSE(b.execute(42, 10));
    CHECK_FALSE(b.cancel(42, 10));
    CHECK_FALSE(b.remove(42));
    CHECK_FALSE(b.replace(42, 43, 10, 10'000'000));
    CHECK(b.open_orders() == 1);
}

TEST_CASE("over-execute clamps to resting size and still cleans up [ladder]") {
    LadderBook b(10'000'000);
    b.add(1, Side::Sell, 100, 10'010'000);

    REQUIRE(b.execute(1, 250));        // feed gap: more than we hold
    CHECK(b.open_orders() == 0);
    CHECK(b.ask_levels() == 0);
}

TEST_CASE("price outside the ladder window is rejected like a duplicate ref [ladder]") {
    LadderBook b(10'000'000);          // window is [8'000'000, 12'000'000]

    CHECK_FALSE(b.add(1, Side::Buy, 100, 7'999'999));   // below the floor
    CHECK_FALSE(b.add(1, Side::Sell, 100, 12'000'001)); // above the ceiling
    CHECK(b.open_orders() == 0);
    CHECK_FALSE(b.best_bid().has_value());
    CHECK_FALSE(b.best_ask().has_value());

    REQUIRE(b.add(1, Side::Buy, 100, 12'000'000));      // ceiling itself is in range
    CHECK(b.best_bid()->price == 12'000'000);
}

TEST_CASE("base_price near UINT32_MAX does not overflow the window into an oversized ladder [ladder]") {
    // Regression test for a real bug: window_high used to compute
    // base_price + static_cast<uint32_t>(base_price*window_pct) entirely in
    // uint32_t, which overflows past UINT32_MAX for a base_price this
    // large. That could land max_price_ below min_price_, and the
    // constructor's unsigned (max_price_ - min_price_) would then underflow
    // to a huge tick count and attempt a multi-gigabyte vector allocation
    // per LadderBook — reachable from an untrusted wire price via
    // pipeline::BookTraits<LadderBook> (see dispatch_to_book.hpp, which
    // additionally clamps this at the call site as the production-facing
    // fix; this test checks the class itself never produces an inverted or
    // absurd range, for any caller, clamped or not).
    //
    // base_price and tick_size are both round multiples of 10'000'000 so
    // base_price lands on the post-snap grid exactly — keeping this test's
    // own allocation to ~130 ticks instead of needing tick_size=1 (which
    // would demonstrate the same fix but at a genuinely multi-gigabyte
    // allocation size, wasteful under CI, especially under ASan's redzones).
    LadderBook b(4'290'000'000u, /*tick_size=*/10'000'000u, /*window_pct=*/0.30);

    // Constructing at all (no crash, no throw, no oversized allocation) is
    // the headline assertion. Functionally: the reference price itself must
    // still be accepted and reported back correctly, proving max_price_
    // saturated (not wrapped) above it rather than inverting below it.
    CHECK(b.add(1, Side::Buy, 100, 4'290'000'000u));
    CHECK(b.best_bid()->price == 4'290'000'000u);
}

TEST_CASE("base_price of 0 with a tiny window still yields a valid, addable ladder [ladder]") {
    LadderBook b(0, /*tick_size=*/1, /*window_pct=*/0.30);
    CHECK(b.add(1, Side::Buy, 100, 0));
    CHECK(b.best_bid()->price == 0);
}
