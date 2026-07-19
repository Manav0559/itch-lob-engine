#include <array>
#include <catch2/catch_test_macros.hpp>

#include "exec/risk_gate.hpp"

using exec::apply;
using exec::ChildOrder;
using exec::ChildOrderQueue;
using exec::kMaxChildOrders;
using exec::OrderType;
using exec::RejectReason;
using exec::RiskGate;
using exec::RiskLimits;
using itch::Side;

namespace {

RiskLimits generous_limits() {
    RiskLimits l;
    l.max_order_shares = 1'000'000;
    l.max_order_notional = 1'000'000'000;
    l.price_collar_bps = 10'000;  // 100% — effectively no collar
    l.max_cumulative_shares = 1'000'000;
    l.max_cumulative_notional = 1'000'000'000;
    return l;
}

}  // namespace

TEST_CASE("an order within every limit passes with None") {
    RiskGate gate(generous_limits());
    const ChildOrder order{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 50};
    CHECK(gate.check(order) == RejectReason::None);
}

TEST_CASE("an order exceeding max_order_shares is rejected as OrderTooLarge") {
    RiskLimits limits = generous_limits();
    limits.max_order_shares = 100;
    RiskGate gate(limits);

    const ChildOrder order{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 101};
    CHECK(gate.check(order) == RejectReason::OrderTooLarge);
}

TEST_CASE("an order exceeding max_order_notional is rejected as NotionalTooLarge") {
    RiskLimits limits = generous_limits();
    limits.max_order_notional = 9'999;  // 100 * 100 = 10'000 will just cross this
    RiskGate gate(limits);

    const ChildOrder order{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100};
    CHECK(gate.check(order) == RejectReason::NotionalTooLarge);
}

TEST_CASE("a Limit order priced outside the collar is rejected as PriceOutOfCollar") {
    RiskLimits limits = generous_limits();
    limits.price_collar_bps = 100;  // 1% of reference price
    RiskGate gate(limits);
    gate.set_reference_price(10'000);

    // 1% of 10'000 is 100, so 10'101 is just outside the collar.
    const ChildOrder order{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 10'101, .shares = 10};
    CHECK(gate.check(order) == RejectReason::PriceOutOfCollar);
}

TEST_CASE("a Limit order within the collar passes") {
    RiskLimits limits = generous_limits();
    limits.price_collar_bps = 100;  // 1% of reference price
    RiskGate gate(limits);
    gate.set_reference_price(10'000);

    const ChildOrder order{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 10'100, .shares = 10};
    CHECK(gate.check(order) == RejectReason::None);
}

TEST_CASE("the collar check is skipped for Market orders") {
    RiskLimits limits = generous_limits();
    limits.price_collar_bps = 1;  // effectively zero tolerance
    RiskGate gate(limits);
    gate.set_reference_price(10'000);

    // price is ignored for a Market order — this would blow the collar if
    // it were checked, but Market orders have no price to collar against.
    const ChildOrder order{.ts = 1, .side = Side::Buy, .type = OrderType::Market, .price = 0, .shares = 10};
    CHECK(gate.check(order) == RejectReason::None);
}

TEST_CASE("the collar check is skipped entirely when no reference price has been set") {
    RiskLimits limits = generous_limits();
    limits.price_collar_bps = 1;
    RiskGate gate(limits);  // reference_price defaults to the 0 "unset" sentinel

    const ChildOrder order{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 999'999, .shares = 10};
    CHECK(gate.check(order) == RejectReason::None);
}

TEST_CASE("cumulative shares crossing the limit trips the switch on the crossing order") {
    RiskLimits limits = generous_limits();
    limits.max_cumulative_shares = 150;
    RiskGate gate(limits);

    const ChildOrder first{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100};
    REQUIRE(gate.check(first) == RejectReason::None);
    gate.record(first);
    CHECK_FALSE(gate.tripped());

    // 100 + 100 = 200 > 150 — this one crosses the cumulative limit.
    const ChildOrder second{.ts = 2, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100};
    CHECK(gate.check(second) == RejectReason::CumulativeSharesExceeded);
    CHECK(gate.tripped());
}

TEST_CASE("cumulative notional crossing the limit trips the switch on the crossing order") {
    RiskLimits limits = generous_limits();
    limits.max_cumulative_notional = 15'000;  // 100 * 100
    RiskGate gate(limits);

    const ChildOrder first{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100};
    REQUIRE(gate.check(first) == RejectReason::None);
    gate.record(first);
    CHECK_FALSE(gate.tripped());

    // Cumulative notional so far is 10'000; this order's 10'000 would push
    // the running total to 20'000, past the 15'000 limit.
    const ChildOrder second{.ts = 2, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100};
    CHECK(gate.check(second) == RejectReason::CumulativeNotionalExceeded);
    CHECK(gate.tripped());
}

TEST_CASE("once tripped every subsequent order is rejected regardless of its own size") {
    RiskLimits limits = generous_limits();
    limits.max_cumulative_shares = 100;
    RiskGate gate(limits);

    const ChildOrder tripper{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 200};
    CHECK(gate.check(tripper) == RejectReason::CumulativeSharesExceeded);
    REQUIRE(gate.tripped());

    // A single share, priced well within every other limit — still rejected
    // purely because the gate is latched.
    const ChildOrder tiny{.ts = 2, .side = Side::Buy, .type = OrderType::Limit, .price = 1, .shares = 1};
    CHECK(gate.check(tiny) == RejectReason::KillSwitchTripped);
}

TEST_CASE("reset clears the tripped state and normal checking resumes") {
    RiskLimits limits = generous_limits();
    limits.max_cumulative_shares = 100;
    RiskGate gate(limits);

    const ChildOrder tripper{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 200};
    CHECK(gate.check(tripper) == RejectReason::CumulativeSharesExceeded);
    REQUIRE(gate.tripped());

    gate.reset();
    CHECK_FALSE(gate.tripped());

    // Cumulative totals were re-zeroed by reset(), so this now passes
    // rather than immediately re-tripping the switch.
    const ChildOrder order{.ts = 2, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 50};
    CHECK(gate.check(order) == RejectReason::None);
}

TEST_CASE("a passing check does not itself update the cumulative totals") {
    RiskLimits limits = generous_limits();
    limits.max_cumulative_shares = 100;
    RiskGate gate(limits);

    const ChildOrder order{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100};
    // Calling check() repeatedly without record() must not accumulate —
    // otherwise a second identical check() would see 200 cumulative shares
    // and trip on a limit of 100.
    CHECK(gate.check(order) == RejectReason::None);
    CHECK(gate.check(order) == RejectReason::None);
    CHECK(gate.check(order) == RejectReason::None);
}

TEST_CASE("apply splits an input queue into accepted and rejected per the gate's rules") {
    RiskLimits limits = generous_limits();
    limits.max_order_shares = 100;
    limits.max_cumulative_shares = 250;
    RiskGate gate(limits);

    ChildOrderQueue in;
    in.push(ChildOrder{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 10, .shares = 50});   // ok
    in.push(ChildOrder{.ts = 2, .side = Side::Buy, .type = OrderType::Limit, .price = 10, .shares = 500});  // too large
    in.push(ChildOrder{.ts = 3, .side = Side::Buy, .type = OrderType::Limit, .price = 10, .shares = 90});   // ok, cum=140
    in.push(ChildOrder{.ts = 4, .side = Side::Buy, .type = OrderType::Limit, .price = 10, .shares = 90});   // ok, cum=230
    in.push(ChildOrder{.ts = 5, .side = Side::Buy, .type = OrderType::Limit, .price = 10, .shares = 90});   // crosses 250 -> trips
    in.push(ChildOrder{.ts = 6, .side = Side::Buy, .type = OrderType::Limit, .price = 10, .shares = 1});    // latched -> rejected

    ChildOrderQueue accepted;
    std::array<RejectReason, kMaxChildOrders> reasons{};
    const std::size_t rejected_count = apply(gate, in, accepted, &reasons);

    REQUIRE(accepted.size() == 3);
    CHECK(accepted.begin()[0].ts == 1);
    CHECK(accepted.begin()[1].ts == 3);
    CHECK(accepted.begin()[2].ts == 4);

    REQUIRE(rejected_count == 3);
    CHECK(reasons[0] == RejectReason::OrderTooLarge);
    CHECK(reasons[1] == RejectReason::CumulativeSharesExceeded);
    CHECK(reasons[2] == RejectReason::KillSwitchTripped);

    CHECK(gate.tripped());
}

TEST_CASE("apply works without a reasons_out argument") {
    RiskLimits limits = generous_limits();
    RiskGate gate(limits);

    ChildOrderQueue in;
    in.push(ChildOrder{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 10, .shares = 50});

    ChildOrderQueue accepted;
    const std::size_t rejected_count = apply(gate, in, accepted);

    CHECK(rejected_count == 0);
    REQUIRE(accepted.size() == 1);
}
