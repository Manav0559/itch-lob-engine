#pragma once
#include <cstdint>
#include <random>
#include <vector>

#include "itch/encode.hpp"
#include "itch/parser.hpp"

// Synthetic session generator, factored out of bench_main.cpp so that
// bench_threaded_main.cpp can replay the exact same byte stream instead of
// carrying its own copy of a 2.2M-message generator — two independent
// copies would be free to silently drift apart, which would quietly break
// the single-threaded-vs-threaded-pipeline comparison the latter exists to
// make.
namespace bench {

constexpr std::uint32_t kSeed = 1'234'567u;
constexpr int kNumSymbols = 9;
constexpr std::size_t kTotalMessages = 2'200'000;
constexpr std::uint32_t kTickSize = 100;  // $0.01, in ITCH's 1/10000-dollar price units

struct SymbolSpec {
    const char* ticker;
    std::uint32_t mid_price;
};

// Mid prices in ITCH price units (dollars * 10000).
constexpr SymbolSpec kSymbols[kNumSymbols] = {
    {"AAPL", 1'500'000},  {"MSFT", 4'200'000}, {"AMZN", 1'785'000},
    {"GOOGL", 1'418'000}, {"META", 5'052'500}, {"NFLX", 6'250'000},
    {"TSLA", 2'485'000},  {"NVDA", 1'187'500}, {"AMD", 1'672'000},
};

struct OpenOrder {
    std::uint64_t ref;
    itch::Side side;
    std::uint32_t price;
    std::uint32_t shares;
};

// Message-type weights approximating a real feed: adds dominate, executes
// follow, cancels/deletes/replaces are progressively rarer.
constexpr char kMsgTypes[] = {'A', 'E', 'C', 'X', 'D', 'U'};
constexpr int kMsgWeights[] = {55, 17, 5, 13, 6, 4};

inline std::uint32_t wiggle_price(std::uint32_t mid, std::mt19937& rng) {
    std::uniform_int_distribution<int> bps(-500, 500);  // +/-5.00%, in 0.01% steps
    const double factor = 1.0 + static_cast<double>(bps(rng)) / 10000.0;
    const auto raw = static_cast<std::uint32_t>(static_cast<double>(mid) * factor);
    return (raw / kTickSize) * kTickSize;
}

inline std::uint32_t lot_shares(std::mt19937& rng) {
    std::uniform_int_distribution<int> lots(1, 20);  // 100..2000 shares, round lots
    return static_cast<std::uint32_t>(lots(rng) * 100);
}

// Builds the wire bytes once so every consumer of this function replays
// identical input. Each symbol keeps a pool of its currently-open orders so
// E/C/X/D/U messages reference real, live refs instead of missing ones — a
// lookup that always fails is a much cheaper (and unrepresentative) code
// path than the mutation being benchmarked.
inline std::vector<std::uint8_t> generate_synthetic_session() {
    using namespace itch::encode;

    std::mt19937 rng(kSeed);
    std::uniform_int_distribution<int> sym_pick(0, kNumSymbols - 1);
    std::discrete_distribution<int> type_pick(std::begin(kMsgWeights), std::end(kMsgWeights));
    std::uniform_int_distribution<int> side_pick(0, 1);

    struct Symbol {
        std::uint16_t locate;
        const char* ticker;
        std::uint32_t mid_price;
        std::vector<OpenOrder> open;
    };
    std::vector<Symbol> symbols;
    symbols.reserve(kNumSymbols);
    for (int i = 0; i < kNumSymbols; ++i)
        symbols.push_back(
            {static_cast<std::uint16_t>(i), kSymbols[i].ticker, kSymbols[i].mid_price, {}});

    std::vector<std::uint8_t> raw;
    raw.reserve(kTotalMessages * 40);  // rough average frame size, just a reserve hint

    std::uint64_t next_ref = 1;
    std::uint64_t next_match = 1;
    constexpr std::uint64_t kBaseTs = 34'200'000'000'000ull;  // 09:30:00, ns since midnight
    constexpr std::uint64_t kTsStep = 9'000'000ull;

    for (std::size_t i = 0; i < kTotalMessages; ++i) {
        Symbol& sym = symbols[static_cast<std::size_t>(sym_pick(rng))];
        const std::uint64_t ts = kBaseTs + i * kTsStep;
        // An empty pool has nothing to execute/cancel/delete/replace against.
        const char t =
            sym.open.empty() ? 'A' : kMsgTypes[static_cast<std::size_t>(type_pick(rng))];

        if (t == 'A') {
            const itch::Side side = side_pick(rng) == 0 ? itch::Side::Buy : itch::Side::Sell;
            const std::uint32_t price = wiggle_price(sym.mid_price, rng);
            const std::uint32_t shares = lot_shares(rng);
            const std::uint64_t ref = next_ref++;
            frame(raw, add_order(sym.locate, ts, ref, side, shares, sym.ticker, price));
            sym.open.push_back({ref, side, price, shares});
            continue;
        }

        std::uniform_int_distribution<std::size_t> idx_pick(0, sym.open.size() - 1);
        const std::size_t idx = idx_pick(rng);
        OpenOrder& o = sym.open[idx];
        auto swap_remove = [&sym](std::size_t i) {
            sym.open[i] = sym.open.back();
            sym.open.pop_back();
        };

        switch (t) {
            case 'E': {
                std::uniform_int_distribution<std::uint32_t> amt(1, o.shares);
                const std::uint32_t qty = amt(rng);
                frame(raw, executed(sym.locate, ts, o.ref, qty, next_match++));
                o.shares -= qty;
                if (o.shares == 0) swap_remove(idx);
                break;
            }
            case 'C': {
                std::uniform_int_distribution<std::uint32_t> amt(1, o.shares);
                const std::uint32_t qty = amt(rng);
                const bool printable = side_pick(rng) == 0;
                frame(raw, executed_price(sym.locate, ts, o.ref, qty, next_match++, printable,
                                          o.price));
                o.shares -= qty;
                if (o.shares == 0) swap_remove(idx);
                break;
            }
            case 'X': {
                std::uniform_int_distribution<std::uint32_t> amt(1, o.shares);
                const std::uint32_t qty = amt(rng);
                frame(raw, cancel(sym.locate, ts, o.ref, qty));
                o.shares -= qty;
                if (o.shares == 0) swap_remove(idx);
                break;
            }
            case 'D': {
                frame(raw, del(sym.locate, ts, o.ref));
                swap_remove(idx);
                break;
            }
            case 'U': {
                const std::uint64_t new_ref = next_ref++;
                const std::uint32_t new_price = wiggle_price(sym.mid_price, rng);
                const std::uint32_t new_shares = lot_shares(rng);
                const itch::Side side = o.side;
                frame(raw, replace(sym.locate, ts, o.ref, new_ref, new_shares, new_price));
                swap_remove(idx);
                sym.open.push_back({new_ref, side, new_price, new_shares});
                break;
            }
            default:
                break;
        }
    }
    return raw;
}

}  // namespace bench
