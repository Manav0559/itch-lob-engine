#include <array>

#include <catch2/catch_test_macros.hpp>

#include "exec/risk_gate.hpp"

using exec::apply;
using exec::ChildOrder;
using exec::ChildOrderQueue;
using exec::OrderType;
using exec::RejectReason;
using exec::RiskGate;
using exec::RiskLimits;
using itch::Side;

namespace {

// Shared baseline: generous enough that any one limit can be tightened per
// test without the others accidentally firing first.
RiskLimits base_limits() {
    RiskLimits limits;
    limits.max_order_shares = 1'000;
    limits.max_order_notional = 50'000;
    limits.price_collar_bps = 100;  // 1%
    limits.max_cumulative_shares = 1'000'000;
    limits.max_cumulative_notional = 1'000'000'000;
    return limits;
}

}  // namespace

TEST_CASE("an order within every limit passes with None") {
    RiskGate gate(base_limits(), /*reference_price=*/100);

    const ChildOrder order{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 400};
    CHECK(gate.check(order) == RejectReason::None);
}

TEST_CASE("an order exceeding max_order_shares is rejected as OrderTooLarge") {
    RiskGate gate(base_limits(), /*reference_price=*/100);

    // Priced low so notional alone (1'001) would pass — isolates the size check.
    const ChildOrder order{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 1, .shares = 1'001};
    CHECK(gate.check(order) == RejectReason::OrderTooLarge);
}

TEST_CASE("an order exceeding max_order_notional is rejected as NotionalTooLarge") {
    RiskGate gate(base_limits(), /*reference_price=*/100);

    // 600 shares is under max_order_shares (1'000), but 600 * 100 = 60'000 > 50'000.
    const ChildOrder order{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 600};
    CHECK(gate.check(order) == RejectReason::NotionalTooLarge);
}

TEST_CASE("a Limit order priced outside the collar is rejected as PriceOutOfCollar") {
    RiskGate gate(base_limits(), /*reference_price=*/100);

    // 1% of a reference of 100 allows 1 tick of deviation; 10 ticks away is well outside it.
    const ChildOrder order{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 110, .shares = 100};
    CHECK(gate.check(order) == RejectReason::PriceOutOfCollar);
}

TEST_CASE("a Limit order within the collar passes") {
    RiskGate gate(base_limits(), /*reference_price=*/100);

    const ChildOrder order{.ts = 1, .side = Side::Sell, .type = OrderType::Limit, .price = 101, .shares = 100};
    CHECK(gate.check(order) == RejectReason::None);
}

TEST_CASE("the collar check is skipped for Market orders") {
    RiskGate gate(base_limits(), /*reference_price=*/100);

    // price = 0 by convention for Market ChildOrders (see exec::Pov's
    // on_trade_tick_impl) — if this were evaluated as a Limit order it would
    // be wildly outside the collar around reference_price 100.
    const ChildOrder order{.ts = 1, .side = Side::Buy, .type = OrderType::Market, .price = 0, .shares = 100};
    CHECK(gate.check(order) == RejectReason::None);
}

TEST_CASE("the collar exemption is Market-only — a Limit order on the same gate is still checked") {
    RiskGate gate(base_limits(), /*reference_price=*/100);

    const ChildOrder market_order{.ts = 1, .side = Side::Buy, .type = OrderType::Market, .price = 0, .shares = 100};
    CHECK(gate.check(market_order) == RejectReason::None);

    const ChildOrder limit_order{.ts = 2, .side = Side::Buy, .type = OrderType::Limit, .price = 110, .shares = 100};
    CHECK(gate.check(limit_order) == RejectReason::PriceOutOfCollar);
}

TEST_CASE("crossing max_cumulative_shares trips the switch on the crossing order") {
    RiskLimits limits = base_limits();
    limits.max_cumulative_shares = 250;
    RiskGate gate(limits, /*reference_price=*/100);

    const ChildOrder a{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100};
    const ChildOrder b{.ts = 2, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100};
    const ChildOrder c{.ts = 3, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100};

    REQUIRE(gate.check(a) == RejectReason::None);
    gate.record(a);
    CHECK_FALSE(gate.tripped());

    REQUIRE(gate.check(b) == RejectReason::None);
    gate.record(b);
    CHECK_FALSE(gate.tripped());  // cumulative now 200 shares, still under 250

    // Cumulative would become 300 — crosses the 250-share limit.
    CHECK(gate.check(c) == RejectReason::CumulativeSharesExceeded);
    CHECK(gate.tripped());
}

TEST_CASE("crossing max_cumulative_notional trips the switch on the crossing order") {
    RiskLimits limits = base_limits();
    limits.max_cumulative_notional = 25'000;
    RiskGate gate(limits, /*reference_price=*/100);

    // 100 shares @ 100 = 10'000 notional each, individually well under
    // max_order_notional (50'000).
    const ChildOrder a{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100};
    const ChildOrder b{.ts = 2, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100};
    const ChildOrder c{.ts = 3, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100};

    REQUIRE(gate.check(a) == RejectReason::None);
    gate.record(a);
    REQUIRE(gate.check(b) == RejectReason::None);
    gate.record(b);
    CHECK_FALSE(gate.tripped());  // cumulative notional now 20'000, under 25'000

    // Cumulative would become 30'000 — crosses the 25'000 limit.
    CHECK(gate.check(c) == RejectReason::CumulativeNotionalExceeded);
    CHECK(gate.tripped());
}

TEST_CASE("once tripped, every subsequent order is rejected with KillSwitchTripped regardless of its own size") {
    RiskLimits limits = base_limits();
    limits.max_cumulative_shares = 100;
    RiskGate gate(limits, /*reference_price=*/100);

    const ChildOrder a{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100};
    REQUIRE(gate.check(a) == RejectReason::None);
    gate.record(a);

    const ChildOrder b{.ts = 2, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 1};
    REQUIRE(gate.check(b) == RejectReason::CumulativeSharesExceeded);
    REQUIRE(gate.tripped());

    // A tiny, otherwise-flawless order — well within every per-order and
    // cumulative limit on its own — is still rejected purely because the
    // gate is latched, proving this is a latch and not a re-evaluated
    // threshold.
    const ChildOrder tiny{.ts = 3, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 1};
    CHECK(gate.check(tiny) == RejectReason::KillSwitchTripped);
}

TEST_CASE("reset() clears the tripped state and normal checking resumes") {
    RiskLimits limits = base_limits();
    limits.max_cumulative_shares = 100;
    RiskGate gate(limits, /*reference_price=*/100);

    const ChildOrder a{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100};
    REQUIRE(gate.check(a) == RejectReason::None);
    gate.record(a);

    const ChildOrder b{.ts = 2, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 1};
    REQUIRE(gate.check(b) == RejectReason::CumulativeSharesExceeded);
    REQUIRE(gate.tripped());

    gate.reset();
    CHECK_FALSE(gate.tripped());

    // Normal checking resumes — including cumulative accounting starting
    // fresh rather than still sitting at the already-blown total (see
    // reset()'s doc comment in risk_gate.hpp for why it zeroes both).
    const ChildOrder c{.ts = 3, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 50};
    CHECK(gate.check(c) == RejectReason::None);
}

TEST_CASE("apply() splits an input queue into accepted vs rejected and reports why") {
    RiskLimits limits = base_limits();
    limits.max_cumulative_shares = 250;
    RiskGate gate(limits, /*reference_price=*/100);

    ChildOrderQueue in;
    // 0: passes.
    in.push(ChildOrder{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100});
    // 1: OrderTooLarge (priced low so notional alone would pass).
    in.push(ChildOrder{.ts = 2, .side = Side::Buy, .type = OrderType::Limit, .price = 1, .shares = 1'001});
    // 2: PriceOutOfCollar.
    in.push(ChildOrder{.ts = 3, .side = Side::Buy, .type = OrderType::Limit, .price = 110, .shares = 50});
    // 3: passes — cumulative shares now 100 + 100 = 200.
    in.push(ChildOrder{.ts = 4, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100});
    // 4: would push cumulative to 300 > 250 — trips the switch.
    in.push(ChildOrder{.ts = 5, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100});
    // 5: tiny, otherwise-fine order — but the gate is now latched.
    in.push(ChildOrder{.ts = 6, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 1});

    ChildOrderQueue accepted;
    std::array<RejectReason, exec::kMaxChildOrders> reasons{};
    const std::size_t accepted_count = apply(gate, in, accepted, &reasons);

    CHECK(accepted_count == 2);
    REQUIRE(accepted.size() == 2);
    CHECK(accepted.begin()[0].ts == 1);
    CHECK(accepted.begin()[1].ts == 4);

    CHECK(reasons[0] == RejectReason::None);
    CHECK(reasons[1] == RejectReason::OrderTooLarge);
    CHECK(reasons[2] == RejectReason::PriceOutOfCollar);
    CHECK(reasons[3] == RejectReason::None);
    CHECK(reasons[4] == RejectReason::CumulativeSharesExceeded);
    CHECK(reasons[5] == RejectReason::KillSwitchTripped);

    CHECK(gate.tripped());
}

TEST_CASE("apply() works without reasons_out") {
    RiskGate gate(base_limits(), /*reference_price=*/100);

    ChildOrderQueue in;
    in.push(ChildOrder{.ts = 1, .side = Side::Buy, .type = OrderType::Limit, .price = 100, .shares = 100});
    in.push(ChildOrder{.ts = 2, .side = Side::Buy, .type = OrderType::Limit, .price = 1, .shares = 1'001});

    ChildOrderQueue accepted;
    const std::size_t accepted_count = apply(gate, in, accepted);

    CHECK(accepted_count == 1);
    REQUIRE(accepted.size() == 1);
    CHECK(accepted.begin()[0].ts == 1);
}
