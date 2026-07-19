#include <catch2/catch_test_macros.hpp>

#include "exec/pov.hpp"

using exec::ChildOrder;
using exec::Pov;
using exec::PovParams;
using exec::TradeTick;
using itch::Side;

TEST_CASE("each child order is participation_bps of its trade's shares") {
    PovParams p;
    p.side = Side::Buy;
    p.max_shares = 100'000;
    p.participation_bps = 1000;  // 10%

    Pov pov(p);

    pov.on_trade_tick_impl(TradeTick{.ts = 1, .price = 10'000'000, .shares = 1'000, .side = Side::Sell});
    REQUIRE(pov.child_orders().size() == 1);
    CHECK(pov.child_orders().begin()[0].shares == 100);  // 10% of 1000

    pov.on_trade_tick_impl(TradeTick{.ts = 2, .price = 10'000'000, .shares = 333, .side = Side::Sell});
    REQUIRE(pov.child_orders().size() == 2);
    CHECK(pov.child_orders().begin()[1].shares == 33);  // 333 * 1000 / 10000, integer rounding

    CHECK(pov.shares_sent() == 133);
}

TEST_CASE("running total never exceeds max_shares even on large prints") {
    PovParams p;
    p.side = Side::Buy;
    p.max_shares = 500;
    p.participation_bps = 5000;  // 50%

    Pov pov(p);

    // 50% of 800 would be 400 shares, within budget.
    pov.on_trade_tick_impl(TradeTick{.ts = 1, .price = 10'000'000, .shares = 800, .side = Side::Sell});
    CHECK(pov.shares_sent() == 400);
    CHECK_FALSE(pov.done());

    // 50% of 800 again would be another 400, but only 100 shares of budget remain.
    pov.on_trade_tick_impl(TradeTick{.ts = 2, .price = 10'000'000, .shares = 800, .side = Side::Sell});
    CHECK(pov.shares_sent() == 500);
    REQUIRE(pov.child_orders().size() == 2);
    CHECK(pov.child_orders().begin()[1].shares == 100);  // clamped to remaining budget

    CHECK(pov.done());
    CHECK(pov.remaining() == 0);
}

TEST_CASE("no orders pushed once budget is exhausted") {
    PovParams p;
    p.side = Side::Buy;
    p.max_shares = 100;
    p.participation_bps = 10'000;  // 100%

    Pov pov(p);

    pov.on_trade_tick_impl(TradeTick{.ts = 1, .price = 10'000'000, .shares = 100, .side = Side::Sell});
    REQUIRE(pov.done());
    REQUIRE(pov.child_orders().size() == 1);

    pov.on_trade_tick_impl(TradeTick{.ts = 2, .price = 10'000'000, .shares = 500, .side = Side::Sell});
    CHECK(pov.child_orders().size() == 1);  // still just the first — budget exhausted, nothing new pushed
    CHECK(pov.shares_sent() == 100);
}

TEST_CASE("no orders pushed once past end_ts") {
    PovParams p;
    p.side = Side::Buy;
    p.max_shares = 100'000;
    p.participation_bps = 1000;
    p.end_ts = 100;

    Pov pov(p);

    pov.on_trade_tick_impl(TradeTick{.ts = 50, .price = 10'000'000, .shares = 1'000, .side = Side::Sell});
    REQUIRE(pov.child_orders().size() == 1);

    pov.on_trade_tick_impl(TradeTick{.ts = 100, .price = 10'000'000, .shares = 1'000, .side = Side::Sell});
    REQUIRE(pov.child_orders().size() == 2);  // exactly at end_ts still counts

    pov.on_trade_tick_impl(TradeTick{.ts = 101, .price = 10'000'000, .shares = 1'000, .side = Side::Sell});
    CHECK(pov.child_orders().size() == 2);  // past end_ts — no third order
}

TEST_CASE("a slice that rounds to zero shares pushes nothing") {
    PovParams p;
    p.side = Side::Buy;
    p.max_shares = 100'000;
    p.participation_bps = 1;  // 0.01%

    Pov pov(p);

    pov.on_trade_tick_impl(TradeTick{.ts = 1, .price = 10'000'000, .shares = 10, .side = Side::Sell});
    CHECK(pov.child_orders().empty());
    CHECK(pov.shares_sent() == 0);
}

TEST_CASE("on_bbo_change_impl is a no-op") {
    PovParams p;
    p.side = Side::Buy;
    p.max_shares = 1'000;
    p.participation_bps = 1000;

    Pov pov(p);
    pov.on_bbo_change_impl(exec::Bbo{.ts = 1, .bid_price = 10'000'000, .bid_shares = 100});

    CHECK(pov.child_orders().empty());
    CHECK(pov.shares_sent() == 0);
}
