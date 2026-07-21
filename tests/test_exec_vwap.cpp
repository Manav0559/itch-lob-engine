#include <cstddef>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "exec/vwap.hpp"

using exec::ChildOrder;
using exec::OrderType;
using exec::Shares;
using exec::Timestamp;
using exec::TradeTick;
using exec::Vwap;
using exec::VwapParams;
using itch::Side;

namespace {

TradeTick make_tick(Timestamp ts, Shares shares, Side side = Side::Buy) {
    return TradeTick{ts, /*price=*/10'000'000, shares, side};
}

// A flat/elapsed-time-weighted schedule, computed independently of Vwap's
// own curve-based target_shares() -- this is exactly the formula Vwap used
// before it grew a historical volume-curve model (see vwap.hpp's header
// comment), kept here only as the "flat baseline" the tests below compare
// the new curve-driven schedule against.
Shares flat_target(Timestamp elapsed, Timestamp span, Shares total) {
    return static_cast<Shares>(static_cast<std::uint64_t>(total) * elapsed / span);
}

}  // namespace

TEST_CASE("vwap sends nothing at start_ts and fills exactly by end_ts") {
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

    Shares last = 0;
    for (Timestamp ts : {1'250'000ULL, 1'500'000ULL, 1'750'000ULL, 2'000'000ULL}) {
        vwap.on_trade_tick_impl(make_tick(ts, 500));
        CHECK(vwap.shares_sent() >= last);  // the schedule never regresses
        last = vwap.shares_sent();
    }

    CHECK(vwap.shares_sent() == 1'000'000);  // now == end_ts: curve is forced exact here
    CHECK(vwap.done());
    CHECK(vwap.market_shares_seen() == 5 * 500);
}

TEST_CASE("vwap still fills exactly by end_ts on a session shorter than the curve's bucket count") {
    // start_ts..end_ts spans fewer ts units than kVolumeCurveBuckets -- every
    // bucket boundary maps to the same handful of timestamps. The schedule
    // still has to behave: monotonic, never overshoot, exact fill at the end.
    VwapParams params;
    params.total_shares = 10;
    params.start_ts = 0;
    params.end_ts = 3;
    Vwap vwap(params);

    vwap.on_trade_tick_impl(make_tick(1, 1));
    const Shares after_1 = vwap.shares_sent();
    CHECK(after_1 <= 10);

    vwap.on_trade_tick_impl(make_tick(2, 1));
    const Shares after_2 = vwap.shares_sent();
    CHECK(after_2 >= after_1);

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
    const Shares sent_before = vwap.shares_sent();
    const Shares seen_before = vwap.market_shares_seen();
    const std::size_t orders_before = vwap.child_orders().size();

    vwap.on_trade_tick_impl(make_tick(2'001, 999'999));  // strictly after end_ts

    CHECK(vwap.shares_sent() == sent_before);
    CHECK(vwap.market_shares_seen() == seen_before);
    CHECK(vwap.child_orders().size() == orders_before);
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
    CHECK(vwap.shares_sent() == 5);              // no further child order, no overshoot
    CHECK(vwap.market_shares_seen() == 2);        // still within [start_ts, end_ts], still counted

    Shares total_pushed = 0;
    for (const ChildOrder& child : vwap.child_orders()) {
        total_pushed += child.shares;
    }
    CHECK(total_pushed == 5);
}

TEST_CASE("vwap runs ahead of a flat baseline schedule during the first half of the session") {
    VwapParams params;
    params.total_shares = 1'000'000;
    params.start_ts = 0;
    params.end_ts = 1'300'000;  // 13 curve buckets x 100,000 ts units each
    Vwap vwap(params);

    vwap.on_trade_tick_impl(make_tick(500'000, 1));  // end of the 5th (of 13) curve buckets

    // The curve's first five buckets (open through late morning) are all
    // heavier than a flat 1/13-per-bucket share, so by this point the
    // curve-driven schedule has sent noticeably more than a flat schedule
    // would have.
    const Shares flat = flat_target(500'000, 1'300'000, 1'000'000);
    CHECK(vwap.shares_sent() > flat);
}

TEST_CASE("vwap falls behind a flat baseline schedule during the mid-day lull") {
    VwapParams params;
    params.total_shares = 1'000'000;
    params.start_ts = 0;
    params.end_ts = 1'300'000;
    Vwap vwap(params);

    vwap.on_trade_tick_impl(make_tick(900'000, 1));  // end of the 9th (of 13) curve buckets

    // Buckets 6-9 sit in the curve's mid-day trough (each lighter than a
    // flat 1/13 share), so the front-loaded early lead has been eaten into
    // by this point and the curve-driven schedule trails a flat schedule.
    const Shares flat = flat_target(900'000, 1'300'000, 1'000'000);
    CHECK(vwap.shares_sent() < flat);
}

TEST_CASE("vwap and a flat baseline schedule converge exactly at end_ts") {
    VwapParams params;
    params.total_shares = 1'000'000;
    params.start_ts = 0;
    params.end_ts = 1'300'000;
    Vwap vwap(params);

    vwap.on_trade_tick_impl(make_tick(1'300'000, 1));

    CHECK(vwap.shares_sent() == params.total_shares);
    CHECK(vwap.shares_sent() == flat_target(1'300'000, 1'300'000, 1'000'000));
}

TEST_CASE("vwap's opening and closing bucket slices are bigger than a mid-day trough bucket slice") {
    VwapParams params;
    params.total_shares = 1'000'000;
    params.start_ts = 0;
    params.end_ts = 1'300'000;
    Vwap vwap(params);

    // First curve bucket: [0, 100'000).
    vwap.on_trade_tick_impl(make_tick(100'000, 1));
    const Shares opening_slice = vwap.shares_sent();

    // One of the mid-day trough buckets: [500'000, 600'000).
    vwap.on_trade_tick_impl(make_tick(500'000, 1));
    const Shares before_trough_bucket = vwap.shares_sent();
    vwap.on_trade_tick_impl(make_tick(600'000, 1));
    const Shares trough_slice = vwap.shares_sent() - before_trough_bucket;

    // Last curve bucket: [1'200'000, 1'300'000).
    vwap.on_trade_tick_impl(make_tick(1'200'000, 1));
    const Shares before_closing_bucket = vwap.shares_sent();
    vwap.on_trade_tick_impl(make_tick(1'300'000, 1));
    const Shares closing_slice = vwap.shares_sent() - before_closing_bucket;

    // The classic U-shape: heavy at the open and close, light mid-day.
    CHECK(opening_slice > trough_slice);
    CHECK(closing_slice > trough_slice);
}
