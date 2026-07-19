#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "book/ladder_book.hpp"
#include "book/order_book.hpp"
#include "itch/encode.hpp"
#include "itch/parser.hpp"

using book::LadderBook;
using book::OrderBook;
using book::Quote;
using itch::Side;

// Full-day invariant suite: replay a synthetic trading day (many symbols,
// thousands of add/execute/cancel/delete/replace events) through OrderBook
// and LadderBook side by side, and cross-check both against a third,
// deliberately-naive reference model built independently in this file (a
// plain locator map, scanned linearly for best bid/ask on demand). Three
// independent implementations of the same book-building rules agreeing at
// every checkpoint is a much stronger signal than any single implementation
// asserting on itself.
//
// The synthetic feed is constructed to never cross a symbol's own book
// (buys and sells are drawn from disjoint price bands around a per-symbol
// mid) and never references a stale/foreign order id, so a real
// implementation bug — not a malformed-input artifact — is the only thing
// that can make the three models disagree.
namespace {

constexpr std::size_t kMaxReportedFailures = 20;
constexpr std::size_t kCheckpointStride = 250;  // snapshot+compare every N messages
constexpr int kNumSymbols = 6;
constexpr int kEventsPerSymbol = 2000;

// Obviously-correct-by-inspection oracle: no aggregates, no incremental
// bookkeeping to get wrong — best bid/ask is recomputed by scanning every
// live order every time it's asked for.
class ReferenceModel {
public:
    bool add(std::uint64_t ref, Side side, std::uint32_t shares, std::uint32_t price) {
        return orders_.try_emplace(ref, Order{shares, price, side}).second;
    }

    bool execute(std::uint64_t ref, std::uint32_t shares) { return reduce(ref, shares); }
    bool cancel(std::uint64_t ref, std::uint32_t shares) { return reduce(ref, shares); }

    bool remove(std::uint64_t ref) { return orders_.erase(ref) > 0; }

    bool replace(std::uint64_t orig, std::uint64_t next, std::uint32_t shares,
                 std::uint32_t price) {
        const auto it = orders_.find(orig);
        if (it == orders_.end()) return false;
        const Side side = it->second.side;
        orders_.erase(it);
        return add(next, side, shares, price);
    }

    std::optional<Quote> best_bid() const { return best(Side::Buy); }
    std::optional<Quote> best_ask() const { return best(Side::Sell); }

    std::size_t open_orders() const { return orders_.size(); }

    std::size_t bid_levels() const { return level_count(Side::Buy); }
    std::size_t ask_levels() const { return level_count(Side::Sell); }

private:
    struct Order {
        std::uint32_t shares;
        std::uint32_t price;
        Side side;
    };

    bool reduce(std::uint64_t ref, std::uint32_t shares) {
        const auto it = orders_.find(ref);
        if (it == orders_.end()) return false;
        Order& o = it->second;
        const std::uint32_t qty = shares < o.shares ? shares : o.shares;
        o.shares -= qty;
        if (o.shares == 0) orders_.erase(it);
        return true;
    }

    std::optional<Quote> best(Side side) const {
        std::optional<std::uint32_t> best_price;
        for (const auto& [ref, o] : orders_) {
            (void)ref;
            if (o.side != side) continue;
            if (!best_price || (side == Side::Buy ? o.price > *best_price
                                                    : o.price < *best_price)) {
                best_price = o.price;
            }
        }
        if (!best_price) return std::nullopt;
        std::uint64_t shares = 0;
        for (const auto& [ref, o] : orders_) {
            (void)ref;
            if (o.side == side && o.price == *best_price) shares += o.shares;
        }
        return Quote{*best_price, shares};
    }

    std::size_t level_count(Side side) const {
        std::vector<std::uint32_t> prices;
        for (const auto& [ref, o] : orders_) {
            (void)ref;
            if (o.side != side) continue;
            if (std::find(prices.begin(), prices.end(), o.price) == prices.end())
                prices.push_back(o.price);
        }
        return prices.size();
    }

    std::unordered_map<std::uint64_t, Order> orders_;
};

// Per-symbol triple: the two production books plus the reference oracle, fed
// the exact same event sequence.
struct SymbolState {
    OrderBook order_book;
    LadderBook ladder_book;
    ReferenceModel reference;

    explicit SymbolState(std::uint32_t mid_price) : ladder_book(mid_price) {}
};

struct Recorder {
    std::unordered_map<std::uint16_t, SymbolState>* books;

    void on_add(const itch::AddOrder& m) {
        SymbolState& s = books->at(m.hdr.locate);
        s.order_book.add(m.ref, m.side, m.shares, m.price);
        s.ladder_book.add(m.ref, m.side, m.shares, m.price);
        s.reference.add(m.ref, m.side, m.shares, m.price);
    }
    void on_execute(const itch::OrderExecuted& m) {
        SymbolState& s = books->at(m.hdr.locate);
        s.order_book.execute(m.ref, m.shares);
        s.ladder_book.execute(m.ref, m.shares);
        s.reference.execute(m.ref, m.shares);
    }
    void on_execute_price(const itch::OrderExecutedPrice&) {}
    void on_cancel(const itch::OrderCancel& m) {
        SymbolState& s = books->at(m.hdr.locate);
        s.order_book.cancel(m.ref, m.canceled);
        s.ladder_book.cancel(m.ref, m.canceled);
        s.reference.cancel(m.ref, m.canceled);
    }
    void on_delete(const itch::OrderDelete& m) {
        SymbolState& s = books->at(m.hdr.locate);
        s.order_book.remove(m.ref);
        s.ladder_book.remove(m.ref);
        s.reference.remove(m.ref);
    }
    void on_replace(const itch::OrderReplace& m) {
        SymbolState& s = books->at(m.hdr.locate);
        s.order_book.replace(m.orig_ref, m.new_ref, m.shares, m.price);
        s.ladder_book.replace(m.orig_ref, m.new_ref, m.shares, m.price);
        s.reference.replace(m.orig_ref, m.new_ref, m.shares, m.price);
    }
    void on_other(char, std::size_t) {}
};

// Quote has no operator== (it's a plain wire-shaped aggregate), so compare
// the optionals field-by-field ourselves.
bool quotes_equal(const std::optional<Quote>& a, const std::optional<Quote>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a.has_value()) return true;
    return a->price == b->price && a->shares == b->shares;
}

std::string describe(const std::optional<Quote>& q) {
    if (!q) return "none";
    std::ostringstream os;
    os << "{price=" << q->price << ", shares=" << q->shares << "}";
    return os.str();
}

// One synthetic-day event, framed and ready to append to the wire stream,
// plus the bookkeeping the generator needs to keep the feed well-formed
// (never references a ref that isn't currently live on its own symbol, and
// never re-prices a replace onto the wrong side of the book).
//
// ITCH's 'U' (replace) carries no side field — the spec fixes the side to
// whatever the original order had, and OrderBook/LadderBook/ReferenceModel
// all honor that by reading it off the order being replaced. So the
// generator must track each live ref's side and pick the replacement's price
// from that same side's band, or it can hand a preserved-buy order a price
// picked for a sell (or vice versa) and manufacture a crossed book that has
// nothing to do with an actual implementation bug.
struct EventGen {
    std::mt19937 rng;
    std::uint16_t locate;
    std::uint32_t mid_price;
    std::uint64_t next_ref;
    std::uint64_t ts = 0;
    std::vector<std::pair<std::uint64_t, Side>>& live;

    EventGen(std::uint32_t seed, std::uint16_t loc, std::uint32_t mid, std::uint64_t ref_base,
             std::vector<std::pair<std::uint64_t, Side>>& live_refs)
        : rng(seed), locate(loc), mid_price(mid), next_ref(ref_base), live(live_refs) {}

    std::uint32_t buy_price() {
        std::uniform_int_distribution<std::uint32_t> d(mid_price - 500'000, mid_price - 1'000);
        return d(rng);
    }
    std::uint32_t sell_price() {
        std::uniform_int_distribution<std::uint32_t> d(mid_price + 1'000, mid_price + 500'000);
        return d(rng);
    }
    std::uint32_t price_for(Side sd) { return sd == Side::Buy ? buy_price() : sell_price(); }
    std::uint32_t shares() {
        std::uniform_int_distribution<std::uint32_t> d(100, 1000);
        return d(rng);
    }
    Side side() {
        std::uniform_int_distribution<int> d(0, 1);
        return d(rng) == 0 ? Side::Buy : Side::Sell;
    }
    std::pair<std::uint64_t, Side> pick_live() {
        std::uniform_int_distribution<std::size_t> d(0, live.size() - 1);
        return live[d(rng)];
    }
    void erase_live(std::uint64_t ref) {
        live.erase(std::find_if(live.begin(), live.end(),
                                [ref](const auto& p) { return p.first == ref; }));
    }

    // Appends one well-formed message to `out`. Returns the message kind
    // actually emitted (falls back to 'A' when no live order exists yet for
    // a mutating op).
    char step(std::vector<std::uint8_t>& out) {
        using namespace itch::encode;
        ts += 1000;
        // Bias toward adds early/whenever the book is thin so cancels,
        // executes, deletes and replaces all get real live targets later.
        std::uniform_int_distribution<int> kind_d(0, live.empty() ? 0 : 4);
        const int kind = kind_d(rng);

        if (kind == 0 || live.empty()) {
            const std::uint64_t ref = next_ref++;
            const Side sd = side();
            const std::uint32_t px = price_for(sd);
            frame(out, add_order(locate, ts, ref, sd, shares(), "SYN", px));
            live.push_back({ref, sd});
            return 'A';
        }
        const auto [ref, live_side] = pick_live();
        switch (kind) {
            case 1: {  // partial execute: never more than a fraction of resting size
                std::uniform_int_distribution<std::uint32_t> qty_d(1, 50);
                frame(out, executed(locate, ts, ref, qty_d(rng), ref));
                return 'E';
            }
            case 2: {
                std::uniform_int_distribution<std::uint32_t> qty_d(1, 50);
                frame(out, cancel(locate, ts, ref, qty_d(rng)));
                return 'X';
            }
            case 3: {
                frame(out, del(locate, ts, ref));
                erase_live(ref);
                return 'D';
            }
            default: {
                // Side is NOT re-rolled: replace preserves the original
                // order's side, so the new price must come from that same
                // side's band or the book legitimately crosses.
                const std::uint64_t next = next_ref++;
                const std::uint32_t px = price_for(live_side);
                frame(out, replace(locate, ts, ref, next, shares(), px));
                erase_live(ref);
                live.push_back({next, live_side});
                return 'U';
            }
        }
    }
};

}  // namespace

TEST_CASE("synthetic trading day: OrderBook, LadderBook and a naive reference model agree "
          "at every checkpoint") {
    std::unordered_map<std::uint16_t, SymbolState> books;
    // The generator's `live` bookkeeping is only a heuristic for picking
    // plausible targets — it can drift (e.g. an over-drained ref lingering
    // in the list). That's harmless here: every event is dispatched to
    // OrderBook, LadderBook and ReferenceModel identically, and all three
    // implement the same "unknown/over-drained ref is a no-op" and
    // "over-execute clamps to zero" rules, so a stale target still can't
    // produce a spurious disagreement — only a genuine divergence between
    // the three implementations can.
    std::vector<std::vector<std::pair<std::uint64_t, Side>>> live_by_symbol(kNumSymbols);
    std::vector<EventGen> gens;
    gens.reserve(kNumSymbols);
    for (int s = 0; s < kNumSymbols; ++s) {
        const std::uint32_t mid = 10'000'000 + static_cast<std::uint32_t>(s) * 250'000;
        books.emplace(static_cast<std::uint16_t>(s), SymbolState{mid});
        gens.emplace_back(/*seed=*/1000u + static_cast<std::uint32_t>(s),
                          /*locate=*/static_cast<std::uint16_t>(s), mid,
                          /*ref_base=*/static_cast<std::uint64_t>(s) << 32,
                          live_by_symbol[s]);
    }

    std::vector<std::uint8_t> wire;
    std::mt19937 scheduler(42);
    std::uniform_int_distribution<int> pick_symbol(0, kNumSymbols - 1);

    std::ostringstream report;
    std::size_t total_failures = 0;
    bool capped = false;
    std::size_t messages_replayed = 0;
    const std::size_t total_messages =
        static_cast<std::size_t>(kNumSymbols) * static_cast<std::size_t>(kEventsPerSymbol);

    Recorder recorder{&books};

    auto run_checkpoint = [&](std::size_t at_message) {
        for (auto& [locate, s] : books) {
            const auto ob_bid = s.order_book.best_bid();
            const auto lb_bid = s.ladder_book.best_bid();
            const auto rf_bid = s.reference.best_bid();
            const auto ob_ask = s.order_book.best_ask();
            const auto lb_ask = s.ladder_book.best_ask();
            const auto rf_ask = s.reference.best_ask();

            std::vector<std::string> mismatches;
            if (!quotes_equal(ob_bid, rf_bid)) {
                mismatches.push_back("OrderBook.best_bid=" + describe(ob_bid) +
                                     " vs reference=" + describe(rf_bid));
            }
            if (!quotes_equal(lb_bid, rf_bid)) {
                mismatches.push_back("LadderBook.best_bid=" + describe(lb_bid) +
                                     " vs reference=" + describe(rf_bid));
            }
            if (!quotes_equal(ob_ask, rf_ask)) {
                mismatches.push_back("OrderBook.best_ask=" + describe(ob_ask) +
                                     " vs reference=" + describe(rf_ask));
            }
            if (!quotes_equal(lb_ask, rf_ask)) {
                mismatches.push_back("LadderBook.best_ask=" + describe(lb_ask) +
                                     " vs reference=" + describe(rf_ask));
            }
            if (s.order_book.open_orders() != s.reference.open_orders()) {
                mismatches.push_back("OrderBook.open_orders=" +
                                     std::to_string(s.order_book.open_orders()) +
                                     " vs reference=" + std::to_string(s.reference.open_orders()));
            }
            if (s.ladder_book.open_orders() != s.reference.open_orders()) {
                mismatches.push_back("LadderBook.open_orders=" +
                                     std::to_string(s.ladder_book.open_orders()) +
                                     " vs reference=" + std::to_string(s.reference.open_orders()));
            }
            if (s.order_book.bid_levels() != s.reference.bid_levels() ||
                s.order_book.ask_levels() != s.reference.ask_levels()) {
                mismatches.push_back(
                    "OrderBook levels bid/ask=" + std::to_string(s.order_book.bid_levels()) + "/" +
                    std::to_string(s.order_book.ask_levels()) + " vs reference=" +
                    std::to_string(s.reference.bid_levels()) + "/" +
                    std::to_string(s.reference.ask_levels()));
            }
            if (s.ladder_book.bid_levels() != s.reference.bid_levels() ||
                s.ladder_book.ask_levels() != s.reference.ask_levels()) {
                mismatches.push_back(
                    "LadderBook levels bid/ask=" + std::to_string(s.ladder_book.bid_levels()) +
                    "/" + std::to_string(s.ladder_book.ask_levels()) + " vs reference=" +
                    std::to_string(s.reference.bid_levels()) + "/" +
                    std::to_string(s.reference.ask_levels()));
            }
            // Crossed book: only meaningful when the generator itself never
            // sends crossing quotes, which is guaranteed by construction.
            if (ob_bid && ob_ask && ob_bid->price >= ob_ask->price) {
                mismatches.push_back("OrderBook crossed: bid=" + describe(ob_bid) +
                                     " ask=" + describe(ob_ask));
            }
            if (lb_bid && lb_ask && lb_bid->price >= lb_ask->price) {
                mismatches.push_back("LadderBook crossed: bid=" + describe(lb_bid) +
                                     " ask=" + describe(lb_ask));
            }

            for (const auto& detail : mismatches) {
                ++total_failures;
                if (total_failures <= kMaxReportedFailures) {
                    report << "checkpoint@msg=" << at_message << " symbol=" << locate << ": "
                           << detail << "\n";
                }
            }
        }
    };

    for (; messages_replayed < total_messages; ++messages_replayed) {
        const int sym = pick_symbol(scheduler);
        gens[static_cast<std::size_t>(sym)].step(wire);
        itch::parse_stream(wire.data(), wire.size(), recorder);
        wire.clear();  // parse_stream is byte-driven; no need to keep replayed frames around

        if ((messages_replayed + 1) % kCheckpointStride == 0) {
            run_checkpoint(messages_replayed + 1);
        }
        if (total_failures > kMaxReportedFailures) {
            capped = true;
            break;  // a systemic bug disagrees at every checkpoint; stop feeding it more of the day
        }
    }
    if (!capped) run_checkpoint(messages_replayed);  // final, end-of-day checkpoint

    if (total_failures > 0) {
        if (capped) {
            report << "... stopped after " << kMaxReportedFailures
                   << " checkpoint disagreements; " << (total_messages - messages_replayed)
                   << " of " << total_messages
                   << " synthetic-day messages were never replayed.\n";
        } else if (total_failures > kMaxReportedFailures) {
            report << "... and " << (total_failures - kMaxReportedFailures)
                   << " more checkpoint disagreements not shown (showing first "
                   << kMaxReportedFailures << ").\n";
        }
        INFO(report.str());
        FAIL_CHECK(total_failures << " checkpoint disagreement(s) across the synthetic day");
    }
}
