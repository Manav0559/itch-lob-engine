#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "book/ladder_book.hpp"
#include "book/order_book.hpp"
#include "exec/almgren_chriss.hpp"
#include "exec/fill_sim.hpp"
#include "exec/pov.hpp"
#include "exec/replay_exec_handler.hpp"
#include "exec/risk_gate.hpp"
#include "exec/twap.hpp"
#include "exec/types.hpp"
#include "exec/vwap.hpp"
#include "io/gzip_source.hpp"
#include "io/mmap_source.hpp"
#include "itch/encode.hpp"
#include "itch/parser.hpp"

// Wires the execution layer (exec::Twap/Vwap/Pov/AlmgrenChriss -> RiskGate ->
// FillSimulator) into a real, book-driven binary. Until this file, exec:: was
// standalone and test-exercised only — docs/architecture.md named the exact
// gap: nothing constructed an exec::Bbo/exec::TradeTick off a live book
// mutation and called ExecutionStrategy::on_bbo_change/on_trade_tick. The
// primitives (book::OrderBook/LadderBook::best_bid()/best_ask(), exec::apply,
// FillSimulator::fill) were already there and already tested in isolation;
// the actual glue lives in include/exec/replay_exec_handler.hpp (header-only,
// so tests/test_replay_exec.cpp can drive it directly) — this file is just
// the CLI: option parsing, strategy selection, mmap/gzip/selftest entry
// points, structured as a sibling of replay_query_main.cpp.
//
// Try it:
//   ./build/replay_exec --selftest --strategy vwap
namespace {

enum class StrategyKind { Twap, Vwap, Pov, AlmgrenChriss };

struct Options {
    std::uint16_t locate = 1;  // matches --selftest's AAPL locate
    StrategyKind strategy = StrategyKind::Twap;
    itch::Side side = itch::Side::Buy;
    exec::Shares shares = 100;
    exec::Timestamp start_ts = 0;
    exec::Timestamp end_ts = 300;
    exec::Timestamp bin_ns = 50;
    std::uint32_t participation_bps = 1000;  // pov only, 10%
    double risk_aversion = 0.0;              // almgren_chriss only
    double volatility = 0.0;                 // almgren_chriss only
    double impact_coefficient = 1.0;         // almgren_chriss only
    exec::Price limit_price = 0;             // 0 == Market child orders
    bool use_map = false;
};

// Generous, hardcoded per-run limits: this binary's purpose is demonstrating
// the strategy -> RiskGate -> FillSimulator pipeline end-to-end, not being a
// configurable risk console, so these aren't CLI flags — a real desk's
// RiskGate would be provisioned per-book from a separate risk config, not
// command-line arguments.
exec::RiskLimits default_risk_limits() {
    exec::RiskLimits limits;
    limits.max_order_shares = 1'000'000;
    limits.max_order_notional = 100'000'000'000ULL;
    limits.price_collar_bps = 10'000;  // 100% — effectively unrestricted by default
    limits.max_cumulative_shares = 10'000'000;
    limits.max_cumulative_notional = 1'000'000'000'000ULL;
    return limits;
}

template <typename BookType, typename Strategy>
void report(const exec::ReplayExecHandler<BookType, Strategy>& h, const exec::FillSimulator& sim,
            const exec::RiskGate& gate, std::size_t bytes, std::size_t frames, double secs) {
    std::printf("bytes            %zu\n", bytes);
    std::printf("frames           %zu\n", frames);
    std::printf("elapsed          %.3f s\n", secs);
    std::printf("locate traded    %u\n", h.locate);
    std::printf("child orders     attempted=%zu accepted=%zu\n", h.total_attempted,
               h.total_accepted);
    for (int i = 1; i <= 6; ++i) {
        const auto count = h.reject_counts[static_cast<std::size_t>(i)];
        if (count > 0)
            std::printf("  rejected %-28s %llu\n",
                       exec::reject_reason_name(static_cast<exec::RejectReason>(i)),
                       static_cast<unsigned long long>(count));
    }
    std::printf("risk gate        %s\n", gate.tripped() ? "TRIPPED" : "ok");
    std::printf("fills            %llu / %llu shares (%.1f%% fill rate)\n",
               static_cast<unsigned long long>(sim.shares_filled()),
               static_cast<unsigned long long>(sim.shares_attempted()), sim.fill_rate() * 100.0);
    // ITCH prices are integers with an implied 4 decimal places (see
    // itch/messages.hpp) — /10000.0 turns the accumulated integer VWAP back
    // into a display dollar amount, at report time only, never on the hot
    // path itself (see FillSimulator::vwap_price).
    std::printf("fill vwap        %.4f\n", sim.vwap_price() / 10'000.0);
}

// data/len is a fully materialized buffer (mmap'd file or --selftest's
// in-memory stream) — parsed via itch::parse_stream, the same entry point
// replay_main.cpp/replay_query_main.cpp use for this path.
template <typename BookType, typename Strategy>
int drive(const Options& opt, Strategy& strategy, const std::uint8_t* data, std::size_t len) {
    exec::RiskGate gate(default_risk_limits());
    exec::FillSimulator sim;
    exec::ReplayExecHandler<BookType, Strategy> handler(opt.locate, strategy, gate, sim);

    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t frames = itch::parse_stream(data, len, handler);
    const auto t1 = std::chrono::steady_clock::now();

    report(handler, sim, gate, len, frames, std::chrono::duration<double>(t1 - t0).count());
    return 0;
}

// io::GzipSource decompresses and dispatches chunk-by-chunk via src.run(h) —
// it never materializes the whole decompressed file, so this path can't
// share drive()'s single data/len buffer; it mirrors replay_query_main.cpp's
// own run_gzip in feeding the handler straight to src.run() instead.
template <typename BookType, typename Strategy>
int drive_gzip(const Options& opt, Strategy& strategy, io::GzipSource& src) {
    exec::RiskGate gate(default_risk_limits());
    exec::FillSimulator sim;
    exec::ReplayExecHandler<BookType, Strategy> handler(opt.locate, strategy, gate, sim);

    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t frames = src.run(handler);
    const auto t1 = std::chrono::steady_clock::now();

    report(handler, sim, gate, src.bytes_decompressed(), frames,
          std::chrono::duration<double>(t1 - t0).count());
    return 0;
}

template <typename BookType>
int dispatch_strategy(const Options& opt, const std::uint8_t* data, std::size_t len) {
    switch (opt.strategy) {
        case StrategyKind::Twap: {
            exec::TwapParams p{opt.side, opt.shares, opt.start_ts, opt.end_ts, opt.bin_ns,
                              opt.limit_price != 0 ? exec::OrderType::Limit : exec::OrderType::Market,
                              opt.limit_price};
            exec::Twap strategy(p);
            return drive<BookType>(opt, strategy, data, len);
        }
        case StrategyKind::Vwap: {
            exec::VwapParams p{opt.side, opt.shares, opt.start_ts, opt.end_ts,
                              opt.limit_price != 0 ? exec::OrderType::Limit : exec::OrderType::Market,
                              opt.limit_price};
            exec::Vwap strategy(p);
            return drive<BookType>(opt, strategy, data, len);
        }
        case StrategyKind::Pov: {
            const exec::PovParams p{opt.side, opt.shares, opt.participation_bps, opt.end_ts};
            exec::Pov strategy(p);
            return drive<BookType>(opt, strategy, data, len);
        }
        case StrategyKind::AlmgrenChriss: {
            exec::AlmgrenChrissParams p{opt.side,
                                        opt.shares,
                                        opt.start_ts,
                                        opt.end_ts,
                                        opt.bin_ns,
                                        opt.risk_aversion,
                                        opt.volatility,
                                        opt.impact_coefficient,
                                        opt.limit_price != 0 ? exec::OrderType::Limit
                                                              : exec::OrderType::Market,
                                        opt.limit_price};
            exec::AlmgrenChriss strategy(p);
            return drive<BookType>(opt, strategy, data, len);
        }
    }
    return 1;  // unreachable — every StrategyKind enumerator is handled above
}

template <typename BookType>
int dispatch_strategy_gzip(const Options& opt, io::GzipSource& src) {
    switch (opt.strategy) {
        case StrategyKind::Twap: {
            exec::TwapParams p{opt.side, opt.shares, opt.start_ts, opt.end_ts, opt.bin_ns,
                              opt.limit_price != 0 ? exec::OrderType::Limit : exec::OrderType::Market,
                              opt.limit_price};
            exec::Twap strategy(p);
            return drive_gzip<BookType>(opt, strategy, src);
        }
        case StrategyKind::Vwap: {
            exec::VwapParams p{opt.side, opt.shares, opt.start_ts, opt.end_ts,
                              opt.limit_price != 0 ? exec::OrderType::Limit : exec::OrderType::Market,
                              opt.limit_price};
            exec::Vwap strategy(p);
            return drive_gzip<BookType>(opt, strategy, src);
        }
        case StrategyKind::Pov: {
            const exec::PovParams p{opt.side, opt.shares, opt.participation_bps, opt.end_ts};
            exec::Pov strategy(p);
            return drive_gzip<BookType>(opt, strategy, src);
        }
        case StrategyKind::AlmgrenChriss: {
            exec::AlmgrenChrissParams p{opt.side,
                                        opt.shares,
                                        opt.start_ts,
                                        opt.end_ts,
                                        opt.bin_ns,
                                        opt.risk_aversion,
                                        opt.volatility,
                                        opt.impact_coefficient,
                                        opt.limit_price != 0 ? exec::OrderType::Limit
                                                              : exec::OrderType::Market,
                                        opt.limit_price};
            exec::AlmgrenChriss strategy(p);
            return drive_gzip<BookType>(opt, strategy, src);
        }
    }
    return 1;  // unreachable — every StrategyKind enumerator is handled above
}

int run_mmap(const Options& opt, const std::string& path) {
    io::MmapSource src(path);
    return opt.use_map ? dispatch_strategy<book::OrderBook>(opt, src.data(), src.size())
                       : dispatch_strategy<book::LadderBook>(opt, src.data(), src.size());
}

int run_gzip(const Options& opt, const std::string& path) {
    io::GzipSource src(path);
    return opt.use_map ? dispatch_strategy_gzip<book::OrderBook>(opt, src)
                       : dispatch_strategy_gzip<book::LadderBook>(opt, src);
}

// Same synthetic session replay_main.cpp/replay_query_main.cpp use, so this
// binary is directly comparable to both: two symbols, adds through replaces,
// one unhandled type ('S') that must be skipped without desyncing the
// stream. locate 1 (AAPL) spans ts 100-170 with a resting bid/ask throughout
// most of it — exactly what a demo run against --locate 1 (the default)
// needs to see BBO changes and at least one trade tick.
std::vector<std::uint8_t> selftest_stream() {
    using namespace itch::encode;
    Msg system_event;
    header(system_event, 'S', 0, 1);
    system_event.push_back('O');

    return stream({
        system_event,
        add_order(1, 100, 1001, itch::Side::Buy, 300, "AAPL", 1'500'000),
        add_order(1, 110, 1002, itch::Side::Buy, 200, "AAPL", 1'499'900),
        add_order(1, 120, 2001, itch::Side::Sell, 500, "AAPL", 1'500'100),
        add_order(2, 130, 3001, itch::Side::Buy, 1000, "MSFT", 4'200'000),
        executed(1, 140, 1001, 300, 90001),           // fills the whole best bid
        cancel(1, 150, 2001, 100),                    // partial cancel, ask stays
        replace(2, 160, 3001, 3002, 800, 4'199'500),  // MSFT bid moves down
        del(1, 170, 1002),                            // AAPL book now ask-only
    });
}

int run_selftest(const Options& opt) {
    const std::vector<std::uint8_t> buf = selftest_stream();
    return opt.use_map ? dispatch_strategy<book::OrderBook>(opt, buf.data(), buf.size())
                       : dispatch_strategy<book::LadderBook>(opt, buf.data(), buf.size());
}

void usage(const char* argv0) {
    std::fprintf(
        stderr,
        "usage: %s [options] <file>            mmap (or gz-stream) and replay a NASDAQ\n"
        "                                       ITCH 5.0 file through one exec:: strategy\n"
        "       %s [options] --selftest         replay a built-in synthetic session\n"
        "\n"
        "Wires exec::Twap/Vwap/Pov/AlmgrenChriss to a live book built off a real ITCH\n"
        "replay: on every mutation of --locate's book, publishes an exec::Bbo (and, on\n"
        "an execute, an exec::TradeTick) to the strategy; drains whatever it pushes into\n"
        "its ChildOrderQueue through exec::RiskGate, then scores fills with\n"
        "exec::FillSimulator. See docs/architecture.md's exec-layer section.\n"
        "\n"
        "options:\n"
        "  --strategy NAME         twap (default) | vwap | pov | almgren_chriss\n"
        "  --locate N              stock-locate to trade (default 1 == AAPL in\n"
        "                         --selftest)\n"
        "  --side buy|sell         (default buy)\n"
        "  --shares N              total_shares (twap/vwap/almgren_chriss) or\n"
        "                         max_shares (pov) for the run (default 100)\n"
        "  --start-ts N            schedule start, ns since midnight (default 0)\n"
        "  --end-ts N              schedule end, ns since midnight (default 300; must\n"
        "                         be > --start-ts)\n"
        "  --bin-ns N              twap/almgren_chriss slice interval (default 50)\n"
        "  --participation-bps N   pov only, basis points of each print (default 1000\n"
        "                         == 10%%)\n"
        "  --risk-aversion X       almgren_chriss only (default 0.0 == uniform/twap-\n"
        "                         like schedule)\n"
        "  --volatility X          almgren_chriss only (default 0.0)\n"
        "  --impact-coefficient X  almgren_chriss only (default 1.0)\n"
        "  --limit-price N         send Limit child orders at this price instead of\n"
        "                         the default Market (0 == Market; a real ITCH price\n"
        "                         is never zero)\n"
        "  --map                   force book::OrderBook instead of the default\n"
        "                         LadderBook (see README) — an explicit A/B option\n",
        argv0, argv0);
}

bool consume_value_flag(int& argc, char** argv, const char* flag, std::string& out) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            out = argv[i + 1];
            for (int j = i; j < argc - 2; ++j) argv[j] = argv[j + 2];
            argc -= 2;
            return true;
        }
    }
    return false;
}

bool consume_bool_flag(int& argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            for (int j = i; j < argc - 1; ++j) argv[j] = argv[j + 1];
            --argc;
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    std::string val;

    opt.use_map = consume_bool_flag(argc, argv, "--map");
    const bool want_selftest = consume_bool_flag(argc, argv, "--selftest");

    if (consume_value_flag(argc, argv, "--strategy", val)) {
        if (val == "twap") opt.strategy = StrategyKind::Twap;
        else if (val == "vwap") opt.strategy = StrategyKind::Vwap;
        else if (val == "pov") opt.strategy = StrategyKind::Pov;
        else if (val == "almgren_chriss") opt.strategy = StrategyKind::AlmgrenChriss;
        else {
            std::fprintf(stderr, "error: unknown --strategy '%s'\n", val.c_str());
            return 2;
        }
    }
    if (consume_value_flag(argc, argv, "--locate", val))
        opt.locate = static_cast<std::uint16_t>(std::atoi(val.c_str()));
    if (consume_value_flag(argc, argv, "--side", val)) {
        if (val == "buy") opt.side = itch::Side::Buy;
        else if (val == "sell") opt.side = itch::Side::Sell;
        else {
            std::fprintf(stderr, "error: --side must be 'buy' or 'sell'\n");
            return 2;
        }
    }
    if (consume_value_flag(argc, argv, "--shares", val))
        opt.shares = static_cast<exec::Shares>(std::strtoull(val.c_str(), nullptr, 10));
    if (consume_value_flag(argc, argv, "--start-ts", val))
        opt.start_ts = static_cast<exec::Timestamp>(std::strtoull(val.c_str(), nullptr, 10));
    if (consume_value_flag(argc, argv, "--end-ts", val))
        opt.end_ts = static_cast<exec::Timestamp>(std::strtoull(val.c_str(), nullptr, 10));
    if (consume_value_flag(argc, argv, "--bin-ns", val))
        opt.bin_ns = static_cast<exec::Timestamp>(std::strtoull(val.c_str(), nullptr, 10));
    if (consume_value_flag(argc, argv, "--participation-bps", val))
        opt.participation_bps = static_cast<std::uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
    if (consume_value_flag(argc, argv, "--risk-aversion", val)) opt.risk_aversion = std::atof(val.c_str());
    if (consume_value_flag(argc, argv, "--volatility", val)) opt.volatility = std::atof(val.c_str());
    if (consume_value_flag(argc, argv, "--impact-coefficient", val))
        opt.impact_coefficient = std::atof(val.c_str());
    if (consume_value_flag(argc, argv, "--limit-price", val))
        opt.limit_price = static_cast<exec::Price>(std::strtoul(val.c_str(), nullptr, 10));

    try {
        if (want_selftest) {
            if (argc != 1) {
                usage(argv[0]);
                return 2;
            }
            return run_selftest(opt);
        }
        if (argc != 2) {
            usage(argv[0]);
            return 2;
        }
        const std::string path = argv[1];
        const bool is_gz = path.size() >= 3 && path.compare(path.size() - 3, 3, ".gz") == 0;
        return is_gz ? run_gzip(opt, path) : run_mmap(opt, path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
