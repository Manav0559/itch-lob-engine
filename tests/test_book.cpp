#include <catch2/catch_test_macros.hpp>

#include "book/order_book.hpp"

using book::OrderBook;
using itch::Side;

TEST_CASE("best bid/ask track adds across levels") {
    OrderBook b;
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

TEST_CASE("partial execute shrinks the level; full execute erases order AND level") {
    OrderBook b;
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

TEST_CASE("emptied best level exposes the next level down") {
    OrderBook b;
    b.add(1, Side::Buy, 100, 10'000'000);
    b.add(2, Side::Buy, 250, 9'990'000);

    REQUIRE(b.remove(1));
    const auto bid = b.best_bid();
    REQUIRE(bid.has_value());
    CHECK(bid->price == 9'990'000);    // a lingering empty level would mask this
    CHECK(bid->shares == 250);
    CHECK(b.bid_levels() == 1);
}

TEST_CASE("two orders at one price: removing one keeps the level with the remainder") {
    OrderBook b;
    b.add(1, Side::Sell, 100, 10'010'000);
    b.add(2, Side::Sell, 400, 10'010'000);

    REQUIRE(b.remove(1));
    const auto ask = b.best_ask();
    REQUIRE(ask.has_value());
    CHECK(ask->shares == 400);
    CHECK(b.ask_levels() == 1);
    CHECK(b.open_orders() == 1);
}

TEST_CASE("partial cancel keeps the order open; delete removes the remainder") {
    OrderBook b;
    b.add(1, Side::Buy, 500, 10'000'000);

    REQUIRE(b.cancel(1, 200));
    CHECK(b.open_orders() == 1);
    CHECK(b.best_bid()->shares == 300);

    REQUIRE(b.remove(1));
    CHECK(b.open_orders() == 0);
    CHECK(b.bid_levels() == 0);
}

TEST_CASE("replace moves size and price, keeps side, retires the old ref") {
    OrderBook b;
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

TEST_CASE("unknown and duplicate refs are reported, never corrupt aggregates") {
    OrderBook b;
    REQUIRE(b.add(1, Side::Buy, 100, 10'000'000));

    CHECK_FALSE(b.add(1, Side::Buy, 999, 10'000'000));  // duplicate add ignored
    CHECK(b.best_bid()->shares == 100);                  // not double-counted

    CHECK_FALSE(b.execute(42, 10));
    CHECK_FALSE(b.cancel(42, 10));
    CHECK_FALSE(b.remove(42));
    CHECK_FALSE(b.replace(42, 43, 10, 10'000'000));
    CHECK(b.open_orders() == 1);
}

TEST_CASE("over-execute clamps to resting size and still cleans up") {
    OrderBook b;
    b.add(1, Side::Sell, 100, 10'010'000);

    REQUIRE(b.execute(1, 250));        // feed gap: more than we hold
    CHECK(b.open_orders() == 0);
    CHECK(b.ask_levels() == 0);
}
