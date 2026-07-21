#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <stdexcept>

#include "exec/almgren_chriss.hpp"
#include "exec/twap.hpp"

using exec::AlmgrenChriss;
using exec::AlmgrenChrissParams;
using exec::ChildOrder;
using exec::OrderType;
using exec::Shares;
using exec::Timestamp;
using exec::Twap;
using exec::TwapParams;
using itch::Side;

namespace {
AlmgrenChrissParams make_params(Shares total_shares, Timestamp start_ts, Timestamp end_ts,
                                 Timestamp bin_ns, double risk_aversion, double volatility = 0.02,
                                 double impact_coefficient = 0.001) {
    AlmgrenChrissParams p;
    p.side = Side::Buy;
    p.total_shares = total_shares;
    p.start_ts = start_ts;
    p.end_ts = end_ts;
    p.bin_ns = bin_ns;
    p.risk_aversion = risk_aversion;
    p.volatility = volatility;
    p.impact_coefficient = impact_coefficient;
    p.child_type = OrderType::Limit;
    p.limit_price = 10'000'000;
    return p;
}

TwapParams make_twap_params(Shares total_shares, Timestamp start_ts, Timestamp end_ts,
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

Shares sum_shares(const AlmgrenChriss& ac) {
    Shares sum = 0;
    for (const auto& co : ac.child_orders()) sum += co.shares;
    return sum;
}
}  // namespace

TEST_CASE("zero risk aversion matches Twap's uniform schedule within integer rounding") {
    AlmgrenChriss ac(make_params(1000, 0, 500, 100, 0.0));
    Twap t(make_twap_params(1000, 0, 500, 100));
    REQUIRE(ac.bin_count() == t.bin_count());
    ac.advance(500);
    t.advance(500);

    auto ac_it = ac.child_orders().begin();
    auto t_it = t.child_orders().begin();
    for (; ac_it != ac.child_orders().end() && t_it != t.child_orders().end(); ++ac_it, ++t_it) {
        CHECK(static_cast<int>(ac_it->shares) - static_cast<int>(t_it->shares) <= 1);
        CHECK(static_cast<int>(t_it->shares) - static_cast<int>(ac_it->shares) <= 1);
    }
    CHECK(sum_shares(ac) == 1000);
    Shares twap_sum = 0;
    for (const auto& co : t.child_orders()) twap_sum += co.shares;
    CHECK(twap_sum == 1000);
}

TEST_CASE("very small risk aversion is close to uniform") {
    AlmgrenChriss ac(make_params(1000, 0, 500, 100, 1e-9));
    REQUIRE(ac.bin_count() == 5);
    ac.advance(500);
    CHECK(sum_shares(ac) == 1000);

    Shares first = 0, last = 0;
    std::size_t i = 0;
    for (const auto& co : ac.child_orders()) {
        if (i == 0) first = co.shares;
        last = co.shares;
        ++i;
    }
    // Within integer rounding of the uniform 200-per-bin split.
    CHECK(first >= 198);
    CHECK(first <= 202);
    CHECK(last >= 198);
    CHECK(last <= 202);
}

TEST_CASE("meaningful risk aversion front-loads the schedule") {
    // Chosen so kappa*horizon lands around 2 — front-loaded but a smooth
    // taper across all 10 bins, not a degenerate step function that dumps
    // everything into bin 1 (which a much larger risk_aversion would).
    AlmgrenChriss ac(make_params(10'000, 0, 1000, 100, 1e-5, 0.02, 0.001));
    REQUIRE(ac.bin_count() == 10);
    ac.advance(1000);
    REQUIRE(sum_shares(ac) == 10'000);

    Shares first = 0, last = 0;
    std::size_t i = 0;
    for (const auto& co : ac.child_orders()) {
        if (i == 0) first = co.shares;
        last = co.shares;
        ++i;
    }
    CHECK(first > last);
}

TEST_CASE("integer schedule always sums exactly to total_shares") {
    struct Case {
        Shares total;
        Timestamp span;
        Timestamp bin_ns;
        double risk_aversion;
    };
    const Case cases[] = {
        {997, 700, 100, 5.0},     // 7 bins, doesn't divide evenly, meaningful risk aversion
        {1, 500, 100, 2.0},       // single share total
        {10'000, 1'000, 3, 0.5},  // bin_ns doesn't divide span evenly -> ceil to 334 bins
        {333, 999, 111, 0.0},     // zero risk aversion, uniform fallback path
        {50'000, 2'000, 200, 50.0},  // large risk aversion, heavily front-loaded
    };
    for (const auto& c : cases) {
        AlmgrenChriss ac(make_params(c.total, 0, c.span, c.bin_ns, c.risk_aversion));
        ac.advance(c.span + c.bin_ns);  // push past the end to flush every bin
        CHECK(ac.done());
        CHECK(sum_shares(ac) == c.total);
    }
}

TEST_CASE("advance crosses the right number of bins at various now") {
    AlmgrenChriss ac(make_params(500, 1000, 1500, 100, 1.0));  // boundaries at 1100..1500
    CHECK(ac.advance(1050) == 0);  // before the first boundary
    CHECK_FALSE(ac.done());
    CHECK(ac.advance(1100) == 1);  // exactly at the first boundary
    CHECK(ac.advance(1250) == 1);  // only crosses the 1200 boundary
    CHECK(ac.advance(1500) == 3);  // one call flushes 1300, 1400 and 1500
    CHECK(ac.done());
    CHECK(ac.advance(9999) == 0);  // nothing left once done
}

TEST_CASE("no orders pushed before start_ts (Almgren-Chriss)") {
    AlmgrenChriss ac(make_params(300, 1000, 1300, 100, 1.0));  // first boundary is 1100
    CHECK(ac.advance(999) == 0);
    CHECK(ac.child_orders().empty());
    CHECK(ac.advance(1000) == 0);  // at start_ts itself, still before the first boundary
    CHECK(ac.child_orders().empty());
}

TEST_CASE("done() is true only once every bin has been pushed (Almgren-Chriss)") {
    AlmgrenChriss ac(make_params(400, 0, 400, 100, 1.0));  // 4 bins at 100,200,300,400
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(ac.done());
        ac.advance(static_cast<Timestamp>((i + 1) * 100));
    }
    CHECK(ac.done());
}

TEST_CASE("on_bbo_change_impl and on_trade_tick_impl push nothing") {
    AlmgrenChriss ac(make_params(1000, 0, 500, 100, 1.0));

    exec::Bbo bbo{};
    bbo.ts = 250;
    bbo.bid_price = 10'000'000;
    bbo.bid_shares = 100;
    bbo.ask_price = 10'010'000;
    bbo.ask_shares = 100;
    ac.on_bbo_change(bbo);
    CHECK(ac.child_orders().empty());

    exec::TradeTick tick{};
    tick.ts = 250;
    tick.price = 10'000'000;
    tick.shares = 500;
    tick.side = Side::Buy;
    ac.on_trade_tick(tick);
    CHECK(ac.child_orders().empty());

    CHECK(ac.bin_count() == 5);
    CHECK_FALSE(ac.done());
}

TEST_CASE("constructor throws instead of relying on assert() for invalid params (Almgren-Chriss)") {
    // See the equivalent Twap test for why this must be a real exception —
    // NDEBUG, set by every build config this project ships (Release,
    // RelWithDebInfo), compiles assert() away entirely.
    CHECK_THROWS_AS(AlmgrenChriss(make_params(1000, 500, 0, 100, 1.0)), std::invalid_argument);
    CHECK_THROWS_AS(AlmgrenChriss(make_params(1000, 0, 500, 0, 1.0)), std::invalid_argument);
    CHECK_THROWS_AS(AlmgrenChriss(make_params(1000, 0, 500, 100, -1.0)), std::invalid_argument);
    CHECK_THROWS_AS(
        AlmgrenChriss(make_params(1000, 0, 500, 100, 1.0, -0.02)), std::invalid_argument);
    CHECK_THROWS_AS(
        AlmgrenChriss(make_params(1000, 0, 500, 100, 1.0, 0.02, 0.0)), std::invalid_argument);
}

TEST_CASE("extreme risk_aversion/volatility never produces NaN or a negative schedule entry") {
    // Regression test for a real bug: before kappa*horizon was clamped,
    // large enough risk_aversion/volatility drove std::sinh(kappa*horizon)
    // to +inf, turning the schedule's inf/inf ratio into NaN — which
    // std::llround + a Shares (uint32_t) cast would then turn into a
    // garbage, silently-accepted share count instead of a visible failure.
    struct Case {
        double risk_aversion;
        double volatility;
        double impact_coefficient;
    };
    const Case cases[] = {
        {1e6, 0.02, 0.001},    // huge risk_aversion
        {1.0, 100.0, 0.001},   // huge volatility
        {1e12, 500.0, 1e-9},   // all three pushed toward the overflow edge at once
    };
    for (const auto& c : cases) {
        AlmgrenChriss ac(
            make_params(50'000, 0, 1000, 100, c.risk_aversion, c.volatility, c.impact_coefficient));
        ac.advance(1000 + 100);
        CHECK(ac.done());
        // If sinh() had overflowed to inf and produced a NaN schedule entry,
        // std::llround(NaN) followed by the Shares (uint32_t) cast is
        // implementation-defined — on this project's target platforms it
        // yields a huge, garbage value, which would blow this sum check far
        // past total_shares rather than landing on it exactly.
        CHECK(sum_shares(ac) == 50'000);
    }
}
