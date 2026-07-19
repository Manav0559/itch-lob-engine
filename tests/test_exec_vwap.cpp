#include <catch2/catch_test_macros.hpp>

#include "exec/vwap.hpp"

using exec::ChildOrder;
using exec::OrderType;
using exec::Shares;
using exec::TradeTick;
using exec::Vwap;
using exec::VwapParams;
using itch::Side;

namespace {

TradeTick make_tick(exec::Timestamp ts, exec::Shares shares, Side side = Side::Buy) {
    return TradeTick{ts, /*price=*/10'000'000, shares, side};
}

}  // namespace

TEST_CASE("vwap tracks the elapsed-time target exactly on evenly-divisible ticks") {
    VwapParams params;
    params.side = Side::Buy;
    params.total_shares = 1'000'000;
    params.start_ts = 1'000'000;
    params.end_ts = 2'000'000;
    params.child_type = OrderType::Limit;
    params.limit_price = 10'000'000;
    Vwap vwap(params);

    vwap.on_trade_tick_impl(make_tick(1'000'000, 500));  // now == start_ts: target == 0
    CHECK(vwap.shares_sent() == 0);
    CHECK(vwap.child_orders().empty());

    vwap.on_trade_tick_impl(make_tick(1'250'000, 500));
    CHECK(vwap.shares_sent() == 250'000);

    vwap.on_trade_tick_impl(make_tick(1'500'000, 500));
    CHECK(vwap.shares_sent() == 500'000);

    vwap.on_trade_tick_impl(make_tick(1'750'000, 500));
    CHECK(vwap.shares_sent() == 750'000);

    vwap.on_trade_tick_impl(make_tick(2'000'000, 500));  // now == end_ts: target == total
    CHECK(vwap.shares_sent() == 1'000'000);
    CHECK(vwap.done());

    CHECK(vwap.child_orders().size() == 4);  // the start_ts tick pushed nothing
    CHECK(vwap.market_shares_seen() == 5 * 500);
}

TEST_CASE("vwap integer division truncates the target but still fills exactly by end_ts") {
    VwapParams params;
    params.total_shares = 10;
    params.start_ts = 0;
    params.end_ts = 3;
    Vwap vwap(params);

    vwap.on_trade_tick_impl(make_tick(1, 1));
    CHECK(vwap.shares_sent() == 3);   // 10 * 1 / 3 == 3 (truncated from 3.33)

    vwap.on_trade_tick_impl(make_tick(2, 1));
    CHECK(vwap.shares_sent() == 6);   // 10 * 2 / 3 == 6 (truncated from 6.67)

    vwap.on_trade_tick_impl(make_tick(3, 1));
    CHECK(vwap.shares_sent() == 10);  // now == end_ts: target == total_shares exactly
    CHECK(vwap.done());
}

TEST_CASE("vwap ignores ticks before start_ts entirely") {
    VwapParams params;
    params.total_shares = 100;
    params.start_ts = 1'000;
    params.end_ts = 2'000;
    Vwap vwap(params);

    vwap.on_trade_tick_impl(make_tick(999, 999'999));

    CHECK(vwap.shares_sent() == 0);
    CHECK(vwap.market_shares_seen() == 0);
    CHECK(vwap.child_orders().empty());
}

TEST_CASE("vwap ignores ticks after end_ts entirely") {
    VwapParams params;
    params.total_shares = 100;
    params.start_ts = 1'000;
    params.end_ts = 2'000;
    Vwap vwap(params);

    vwap.on_trade_tick_impl(make_tick(1'500, 40));
    CHECK(vwap.shares_sent() == 40);
    const auto seen_before = vwap.market_shares_seen();
    const auto sent_before = vwap.child_orders().size();

    vwap.on_trade_tick_impl(make_tick(2'001, 999'999));  // strictly after end_ts

    CHECK(vwap.shares_sent() == 40);
    CHECK(vwap.market_shares_seen() == seen_before);
    CHECK(vwap.child_orders().size() == sent_before);
}

TEST_CASE("vwap never sends more than total_shares and stops once done") {
    VwapParams params;
    params.total_shares = 5;
    params.start_ts = 0;
    params.end_ts = 10;
    Vwap vwap(params);

    vwap.on_trade_tick_impl(make_tick(10, 1));  // jump straight to end_ts
    CHECK(vwap.shares_sent() == 5);
    CHECK(vwap.done());

    vwap.on_trade_tick_impl(make_tick(10, 1));  // another tick at the same ts
    CHECK(vwap.shares_sent() == 5);             // no further child order, no overshoot
    CHECK(vwap.market_shares_seen() == 2);       // still within [start_ts, end_ts], still counted

    Shares total_pushed = 0;
    for (const ChildOrder& child : vwap.child_orders()) {
        total_pushed += child.shares;
    }
    CHECK(total_pushed == 5);
}
