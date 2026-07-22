#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "book/ladder_book.hpp"
#include "exec/fill_sim.hpp"
#include "exec/replay_exec_handler.hpp"
#include "exec/risk_gate.hpp"
#include "exec/twap.hpp"
#include "exec/vwap.hpp"
#include "itch/encode.hpp"
#include "itch/parser.hpp"

// End-to-end coverage for the exec-layer wiring in
// include/exec/replay_exec_handler.hpp: drives the same synthetic session
// src/replay_exec_main.cpp's --selftest uses through a real
// pipeline::BookBuilder<LadderBook>-backed handler, asserting the same kind
// of invariants tests/test_exec_twap.cpp, tests/test_exec_vwap.cpp,
// tests/test_risk_gate.cpp and tests/test_fill_sim.cpp already assert on
// each piece in isolation — now off real book mutations instead of
// hand-built Bbo/TradeTick structs.

using exec::ReplayExecHandler;
using itch::Side;

namespace {

// Same session src/replay_exec_main.cpp's selftest_stream() and
// replay_main.cpp's selftest() use: locate 1 (AAPL) gets a resting bid/ask
// from ts 100, a 300-share execute at ts 140 (fills the whole best bid), a
// partial cancel at ts 150, and a delete at ts 170 that leaves AAPL
// ask-only. Locate 2 (MSFT) is untouched by the strategy in every test below
// (they all trade locate 1) — its replace at ts 160 exists only to confirm
// the handler doesn't misattribute a TradeTick to the wrong locate.
std::vector<std::uint8_t> aapl_msft_session() {
    using namespace itch::encode;
    Msg system_event;
    header(system_event, 'S', 0, 1);
    system_event.push_back('O');

    return stream({
        system_event,
        add_order(1, 100, 1001, Side::Buy, 300, "AAPL", 1'500'000),
        add_order(1, 110, 1002, Side::Buy, 200, "AAPL", 1'499'900),
        add_order(1, 120, 2001, Side::Sell, 500, "AAPL", 1'500'100),
        add_order(2, 130, 3001, Side::Buy, 1000, "MSFT", 4'200'000),
        executed(1, 140, 1001, 300, 90001),
        cancel(1, 150, 2001, 100),
        replace(2, 160, 3001, 3002, 800, 4'199'500),
        del(1, 170, 1002),
    });
}

exec::RiskLimits generous_limits() {
    exec::RiskLimits limits;
    limits.max_order_shares = 1'000'000;
    limits.max_order_notional = 100'000'000'000ULL;
    limits.price_collar_bps = 10'000;
    limits.max_cumulative_shares = 10'000'000;
    limits.max_cumulative_notional = 1'000'000'000'000ULL;
    return limits;
}

}  // namespace

TEST_CASE("Twap end-to-end through ReplayExecHandler: schedule advances off the tape clock and fills") {
    const std::vector<std::uint8_t> buf = aapl_msft_session();

    const exec::TwapParams params{Side::Buy, /*total_shares=*/100, /*start_ts=*/0,
                                  /*end_ts=*/300, /*bin_ns=*/50, exec::OrderType::Market, 0};
    exec::Twap twap(params);
    exec::RiskGate gate(generous_limits());
    exec::FillSimulator sim;
    ReplayExecHandler<book::LadderBook, exec::Twap> handler(1, twap, gate, sim);

    const std::size_t frames = itch::parse_stream(buf.data(), buf.size(), handler);
    CHECK(frames == 9);  // system event + 4 adds + executed + cancel + replace + delete

    // Twap's schedule spans bins at 50,100,...,300 (6 bins for a 300ns
    // window at bin_ns=50) — every message in the session has ts <= 170, so
    // advance() only ever gets driven up to ts=170 (bins through 150), never
    // reaching the final bin(s). At least one bin boundary (50, 100, 150)
    // is still crossed by ts=170, so at least one child order must have
    // been attempted.
    CHECK(handler.total_attempted > 0);
    CHECK(handler.total_accepted == handler.total_attempted);  // generous_limits() never rejects
    CHECK_FALSE(gate.tripped());

    // Market children fill at last_trade_price_, which the single 'E' at
    // ts=140 (price 1,500,000 — AAPL's original resting bid) sets once the
    // first bin boundary at/after ts=140 is crossed.
    CHECK(sim.shares_attempted() > 0);
    CHECK(sim.shares_filled() <= sim.shares_attempted());
}

TEST_CASE("Vwap end-to-end through ReplayExecHandler: the AAPL execute produces exactly one child order") {
    const std::vector<std::uint8_t> buf = aapl_msft_session();

    const exec::VwapParams params{Side::Buy, /*total_shares=*/100, /*start_ts=*/0,
                                  /*end_ts=*/300, exec::OrderType::Market, 0};
    exec::Vwap vwap(params);
    exec::RiskGate gate(generous_limits());
    exec::FillSimulator sim;
    ReplayExecHandler<book::LadderBook, exec::Vwap> handler(1, vwap, gate, sim);

    itch::parse_stream(buf.data(), buf.size(), handler);

    // Vwap only reacts to on_trade_tick — locate 1 has exactly one execute
    // (ts=140, 300 shares), and locate 2's replace (not an execute) must not
    // produce a tick at all: this is the misattribution guard the class
    // comment on aapl_msft_session() describes.
    CHECK(handler.total_attempted == 1);
    CHECK(handler.total_accepted == 1);
    CHECK(vwap.market_shares_seen() == 300);
    CHECK(sim.shares_filled() == vwap.shares_sent());
    CHECK_FALSE(gate.tripped());
}

TEST_CASE("ReplayExecHandler routes RiskGate rejections instead of silently dropping child orders") {
    const std::vector<std::uint8_t> buf = aapl_msft_session();

    const exec::TwapParams params{Side::Buy, /*total_shares=*/100, /*start_ts=*/0,
                                  /*end_ts=*/300, /*bin_ns=*/50, exec::OrderType::Market, 0};
    exec::Twap twap(params);

    // Tight enough that every child (100/6 ~= 17 shares per bin) trips
    // OrderTooLarge, but loose enough it doesn't affect the other tests'
    // shared generous_limits().
    exec::RiskLimits tight = generous_limits();
    tight.max_order_shares = 1;
    exec::RiskGate gate(tight);
    exec::FillSimulator sim;
    ReplayExecHandler<book::LadderBook, exec::Twap> handler(1, twap, gate, sim);

    itch::parse_stream(buf.data(), buf.size(), handler);

    CHECK(handler.total_attempted > 0);
    CHECK(handler.total_accepted == 0);
    CHECK(handler.reject_counts[static_cast<std::size_t>(exec::RejectReason::OrderTooLarge)] ==
         handler.total_attempted);
    CHECK(sim.shares_attempted() == 0);  // nothing accepted, so nothing ever reached the simulator
    CHECK_FALSE(gate.tripped());         // per-order breaches don't latch the kill switch
}
