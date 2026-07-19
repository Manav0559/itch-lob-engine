#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "exec/fill_sim.hpp"
#include "exec/twap.hpp"

using Catch::Approx;
using exec::Bbo;
using exec::ChildOrder;
using exec::Fill;
using exec::FillSimulator;
using exec::OrderType;
using exec::Twap;
using exec::TwapParams;
using itch::Side;

TEST_CASE("a marketable buy limit fills immediately at the ask") {
    FillSimulator sim;
    sim.on_bbo_change(Bbo{.ts = 1, .bid_price = 99, .bid_shares = 100, .ask_price = 100, .ask_shares = 100});

    // Priced above the ask — marketable.
    const ChildOrder order{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 101, .shares = 50};
    REQUIRE(sim.fill(order));

    REQUIRE(sim.fills().size() == 1);
    const Fill& f = sim.fills().begin()[0];
    CHECK(f.ts == 1);
    CHECK(f.side == Side::Buy);
    CHECK(f.price == 100);  // fills at the ask, not the limit price
    CHECK(f.shares == 50);

    CHECK(sim.shares_filled() == 50);
    CHECK(sim.shares_attempted() == 50);
}

TEST_CASE("a marketable sell limit fills immediately at the bid") {
    FillSimulator sim;
    sim.on_bbo_change(Bbo{.ts = 1, .bid_price = 100, .bid_shares = 100, .ask_price = 101, .ask_shares = 100});

    // Priced below the bid — marketable.
    const ChildOrder order{.ts = 1, .side = Side::Sell, .type = OrderType::Limit, .price = 99, .shares = 75};
    REQUIRE(sim.fill(order));

    REQUIRE(sim.fills().size() == 1);
    const Fill& f = sim.fills().begin()[0];
    CHECK(f.price == 100);  // fills at the bid
    CHECK(f.shares == 75);
}

TEST_CASE("a non-marketable limit produces no fill") {
    FillSimulator sim;
    sim.on_bbo_change(Bbo{.ts = 1, .bid_price = 99, .bid_shares = 100, .ask_price = 102, .ask_shares = 100});

    // Priced below the ask — not marketable, and this model does not rest it.
    const ChildOrder order{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 101, .shares = 50};
    CHECK_FALSE(sim.fill(order));

    CHECK(sim.fills().empty());
    CHECK(sim.shares_filled() == 0);
    CHECK(sim.shares_attempted() == 50);  // attempted, but not filled
}

TEST_CASE("a limit exactly at the touch is marketable") {
    FillSimulator sim;
    sim.on_bbo_change(Bbo{.ts = 1, .bid_price = 99, .bid_shares = 100, .ask_price = 100, .ask_shares = 100});

    const ChildOrder buy_at_touch{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 10};
    CHECK(sim.fill(buy_at_touch));

    const ChildOrder sell_at_touch{.ts = 1, .side = Side::Sell, .type = OrderType::Limit, .price = 99, .shares = 10};
    CHECK(sim.fill(sell_at_touch));
}

TEST_CASE("a market order fills at the triggering tick's price") {
    FillSimulator sim;
    sim.on_trade_tick(exec::TradeTick{.ts = 5, .price = 250, .shares = 1'000, .side = Side::Sell});

    const ChildOrder order{.ts = 5, .side = Side::Buy, .type = OrderType::Market, .price = 0, .shares = 300};
    REQUIRE(sim.fill(order));

    REQUIRE(sim.fills().size() == 1);
    const Fill& f = sim.fills().begin()[0];
    CHECK(f.price == 250);
    CHECK(f.shares == 300);
}

TEST_CASE("a market order pushed before any trade tick does not fill") {
    FillSimulator sim;  // last_trade_price_ is still the zero sentinel
    const ChildOrder order{.ts = 1, .side = Side::Buy, .type = OrderType::Market, .price = 0, .shares = 10};
    CHECK_FALSE(sim.fill(order));
    CHECK(sim.shares_filled() == 0);
}

TEST_CASE("vwap arithmetic is exact on hand-computed values") {
    FillSimulator sim;

    // Fill 1: 200 shares @ 10'000
    sim.on_bbo_change(Bbo{.ts = 1, .bid_price = 9'900, .bid_shares = 500, .ask_price = 10'000, .ask_shares = 500});
    REQUIRE(sim.fill(ChildOrder{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 10'000, .shares = 200}));

    // Fill 2: 300 shares @ 10'010
    sim.on_bbo_change(Bbo{.ts = 2, .bid_price = 9'900, .bid_shares = 500, .ask_price = 10'010, .ask_shares = 500});
    REQUIRE(sim.fill(ChildOrder{.ts = 2, .side = Side::Buy, .type = OrderType::Limit, .price = 10'010, .shares = 300}));

    // Fill 3: 500 shares @ 9'990 (market, off a print)
    sim.on_trade_tick(exec::TradeTick{.ts = 3, .price = 9'990, .shares = 500, .side = Side::Sell});
    REQUIRE(sim.fill(ChildOrder{.ts = 3, .side = Side::Buy, .type = OrderType::Market, .price = 0, .shares = 500}));

    // Hand-computed: (200*10000 + 300*10010 + 500*9990) / 1000
    //              = (2'000'000 + 3'003'000 + 4'995'000) / 1000
    //              = 9'998'000 / 1000 = 9998.0
    CHECK(sim.shares_filled() == 1'000);
    CHECK(sim.shares_attempted() == 1'000);
    CHECK(sim.vwap_price() == Approx(9998.0));
    CHECK(sim.fill_rate() == Approx(1.0));
}

TEST_CASE("fill_rate reflects partial completion when not everything is fillable") {
    FillSimulator sim;
    sim.on_bbo_change(Bbo{.ts = 1, .bid_price = 99, .bid_shares = 100, .ask_price = 100, .ask_shares = 100});

    // Marketable: fills.
    REQUIRE(sim.fill(ChildOrder{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 40}));
    // Not marketable: does not fill.
    CHECK_FALSE(sim.fill(ChildOrder{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 95, .shares = 60}));

    CHECK(sim.shares_attempted() == 100);
    CHECK(sim.shares_filled() == 40);
    CHECK(sim.fill_rate() == Approx(0.4));
}

TEST_CASE("fill_rate and vwap_price are zero before anything is attempted") {
    FillSimulator sim;
    CHECK(sim.fill_rate() == Approx(0.0));
    CHECK(sim.vwap_price() == Approx(0.0));
    CHECK(sim.shares_attempted() == 0);
    CHECK(sim.shares_filled() == 0);
}

TEST_CASE("Twap child orders replayed through FillSimulator match a hand-computed fill scenario") {
    TwapParams p;
    p.side = Side::Buy;
    p.total_shares = 300;
    p.start_ts = 0;
    p.end_ts = 300;
    p.bin_ns = 100;  // 3 bins of 100 shares, boundaries at 100/200/300
    p.child_type = OrderType::Limit;
    p.limit_price = 101;
    Twap t(p);

    FillSimulator sim;

    // Bin 1 (ts=100): ask is 102 — the 101 limit is not marketable.
    sim.on_bbo_change(Bbo{.ts = 100, .bid_price = 98, .bid_shares = 50, .ask_price = 102, .ask_shares = 50});
    t.advance(100);
    REQUIRE(t.child_orders().size() == 1);
    CHECK_FALSE(sim.fill(t.child_orders().begin()[0]));

    // Bin 2 (ts=200): ask drops to 100 — marketable, fills at 100.
    sim.on_bbo_change(Bbo{.ts = 200, .bid_price = 99, .bid_shares = 50, .ask_price = 100, .ask_shares = 50});
    t.advance(200);
    REQUIRE(t.child_orders().size() == 2);
    CHECK(sim.fill(t.child_orders().begin()[1]));

    // Bin 3 (ts=300): ask at 101 — exactly at the limit, marketable at 101.
    sim.on_bbo_change(Bbo{.ts = 300, .bid_price = 100, .bid_shares = 50, .ask_price = 101, .ask_shares = 50});
    t.advance(300);
    REQUIRE(t.child_orders().size() == 3);
    CHECK(sim.fill(t.child_orders().begin()[2]));

    CHECK(t.done());
    CHECK(sim.shares_attempted() == 300);   // all three 100-share bins attempted
    CHECK(sim.shares_filled() == 200);      // only bins 2 and 3 were marketable

    // Hand-computed: (100*100 + 100*101) / 200 = (10'000 + 10'100) / 200 = 100.5
    CHECK(sim.vwap_price() == Approx(100.5));
    CHECK(sim.fill_rate() == Approx(200.0 / 300.0));
}
