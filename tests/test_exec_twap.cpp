#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

#include "exec/twap.hpp"

using exec::ChildOrder;
using exec::OrderType;
using exec::Shares;
using exec::Timestamp;
using exec::Twap;
using exec::TwapParams;
using itch::Side;

namespace {
TwapParams make_params(Shares total_shares, Timestamp start_ts, Timestamp end_ts,
                        Timestamp bin_ns) {
    TwapParams p;
    p.side = Side::Buy;
    p.total_shares = total_shares;
    p.start_ts = start_ts;
    p.end_ts = end_ts;
    p.bin_ns = bin_ns;
    p.child_type = OrderType::Limit;
    p.limit_price = 10'000'000;
    return p;
}

Shares sum_shares(const Twap& t) {
    Shares sum = 0;
    for (const auto& co : t.child_orders()) sum += co.shares;
    return sum;
}
}  // namespace

TEST_CASE("schedule sums to total_shares exactly when it divides evenly") {
    Twap t(make_params(1000, 0, 500, 100));  // 5 bins, 200 each
    REQUIRE(t.bin_count() == 5);
    t.advance(500);
    CHECK(sum_shares(t) == 1000);
    for (const auto& co : t.child_orders()) CHECK(co.shares == 200);
}

TEST_CASE("remainder is distributed across the leading bins, not dropped") {
    Twap t(make_params(1000, 0, 300, 100));  // 3 bins: 334, 333, 333
    REQUIRE(t.bin_count() == 3);
    t.advance(300);
    const Shares expected[] = {334, 333, 333};
    std::size_t i = 0;
    for (const auto& co : t.child_orders()) CHECK(co.shares == expected[i++]);
    CHECK(sum_shares(t) == 1000);
}

TEST_CASE("schedule sums to total_shares exactly across bin counts that don't divide evenly") {
    struct Case {
        Shares total;
        Timestamp span;
        Timestamp bin_ns;
    };
    const Case cases[] = {
        {997, 700, 100},    // 7 bins, 997 % 7 != 0
        {1, 500, 100},      // 5 bins, single share total
        {10'000, 1'000, 3}, // bin_ns doesn't divide span evenly -> ceil to 334 bins
        {333, 999, 111},    // 9 bins evenly, share count not evenly divisible
    };
    for (const auto& c : cases) {
        Twap t(make_params(c.total, 0, c.span, c.bin_ns));
        t.advance(c.span + c.bin_ns);  // push past the end to flush every bin
        CHECK(t.done());
        CHECK(sum_shares(t) == c.total);
    }
}

TEST_CASE("advance crosses the right number of bins at various `now`") {
    Twap t(make_params(500, 1000, 1500, 100));  // boundaries at 1100,1200,1300,1400,1500
    CHECK(t.advance(1050) == 0);   // before the first boundary
    CHECK_FALSE(t.done());
    CHECK(t.advance(1100) == 1);   // exactly at the first boundary
    CHECK(t.advance(1250) == 1);   // only crosses the 1200 boundary
    CHECK(t.advance(1500) == 3);   // one call flushes 1300, 1400 and 1500
    CHECK(t.done());
    CHECK(t.advance(9999) == 0);   // nothing left once done
}

TEST_CASE("no orders pushed before start_ts") {
    Twap t(make_params(300, 1000, 1300, 100));  // first boundary is 1100
    CHECK(t.advance(999) == 0);
    CHECK(t.child_orders().empty());
    CHECK(t.advance(1000) == 0);  // at start_ts itself, still before the first boundary
    CHECK(t.child_orders().empty());
}

TEST_CASE("done() is true only once every bin has been pushed") {
    Twap t(make_params(400, 0, 400, 100));  // 4 bins at 100,200,300,400
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(t.done());
        t.advance(static_cast<Timestamp>((i + 1) * 100));
    }
    CHECK(t.done());
}

TEST_CASE("advance stops early without erroring once the child order queue is full") {
    Twap t(make_params(300, 0, 300, 100));  // 3 bins, all due by now=300
    REQUIRE(t.bin_count() == 3);

    // Force the full() branch via the mutable accessor rather than relying
    // on kMaxScheduleBins == kMaxChildOrders — advance() must bail out on a
    // full queue regardless of why it's full.
    ChildOrder filler{};
    while (!t.child_orders().full()) t.child_orders().push(filler);

    const std::size_t pushed = t.advance(300);
    CHECK(pushed == 0);
    CHECK_FALSE(t.done());  // no bin was actually consumed
    CHECK(t.bin_count() == 3);
}

TEST_CASE("pushed child orders carry the configured side, type and limit price") {
    TwapParams p = make_params(600, 0, 300, 100);
    p.side = Side::Sell;
    p.child_type = OrderType::Limit;
    p.limit_price = 10'050'000;
    Twap t(p);
    t.advance(300);
    REQUIRE_FALSE(t.child_orders().empty());
    for (const auto& co : t.child_orders()) {
        CHECK(co.side == Side::Sell);
        CHECK(co.type == OrderType::Limit);
        CHECK(co.price == 10'050'000);
    }
}

TEST_CASE("market child orders carry the configured type") {
    TwapParams p = make_params(300, 0, 300, 100);
    p.child_type = OrderType::Market;
    Twap t(p);
    t.advance(300);
    REQUIRE_FALSE(t.child_orders().empty());
    for (const auto& co : t.child_orders()) CHECK(co.type == OrderType::Market);
}

TEST_CASE("on_bbo_change_impl and on_trade_tick_impl push nothing — Twap is time-driven only") {
    Twap t(make_params(1000, 0, 500, 100));

    exec::Bbo bbo{};
    bbo.ts = 250;
    bbo.bid_price = 10'000'000;
    bbo.bid_shares = 100;
    bbo.ask_price = 10'010'000;
    bbo.ask_shares = 100;
    t.on_bbo_change(bbo);
    CHECK(t.child_orders().empty());

    exec::TradeTick tick{};
    tick.ts = 250;
    tick.price = 10'000'000;
    tick.shares = 500;
    tick.side = Side::Buy;
    t.on_trade_tick(tick);
    CHECK(t.child_orders().empty());

    CHECK(t.bin_count() == 5);
    CHECK_FALSE(t.done());
}

TEST_CASE("constructor throws instead of relying on assert() for invalid params") {
    // Regression test for a real bug: assert() is compiled to a no-op under
    // NDEBUG, which every configuration this project builds (Release,
    // RelWithDebInfo) defines. Before this was fixed, TwapParams's own
    // default bin_ns=1 against a multi-second span would silently overrun
    // the fixed-size schedule_ std::array instead of failing loudly here.
    CHECK_THROWS_AS(Twap(make_params(1000, 500, 0, 100)), std::invalid_argument);  // start >= end
    CHECK_THROWS_AS(Twap(make_params(1000, 0, 500, 0)), std::invalid_argument);    // bin_ns == 0

    TwapParams huge_span;
    huge_span.total_shares = 1000;
    huge_span.start_ts = 0;
    huge_span.end_ts = 1'000'000'000;  // 1 second, in ns
    huge_span.bin_ns = 1;              // TwapParams's own default — 1e9 bins, way past kMaxScheduleBins
    CHECK_THROWS_AS(Twap(huge_span), std::invalid_argument);
}
