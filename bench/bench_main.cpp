// Head-to-head latency benchmark: OrderBook (std::map ladders) vs LadderBook
// (flat tick-indexed ladders) replaying the identical byte stream. Absent a
// real NASDAQ file on the command line, the stream is a synthetic multi-
// symbol session generated in-process with a fixed RNG seed, so the numbers
// this binary prints are reproducible by anyone who clones the repo — no
// exchange data required.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "book/ladder_book.hpp"
#include "book/order_book.hpp"
#include "io/gzip_source.hpp"
#include "io/mmap_source.hpp"
#include "itch/encode.hpp"
#include "itch/parser.hpp"

namespace {

// ---------------------------------------------------------------------
// Synthetic session generator
// ---------------------------------------------------------------------
// Floating point below is generation/reporting code, not the parsing/book
// hot path the project's no-floating-point rule targets — the hot path
// being measured is exactly the one call into book.add/execute/... that
// each Handler callback times.

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

std::uint32_t wiggle_price(std::uint32_t mid, std::mt19937& rng) {
    std::uniform_int_distribution<int> bps(-500, 500);  // +/-5.00%, in 0.01% steps
    const double factor = 1.0 + static_cast<double>(bps(rng)) / 10000.0;
    const auto raw = static_cast<std::uint32_t>(static_cast<double>(mid) * factor);
    return (raw / kTickSize) * kTickSize;
}

std::uint32_t lot_shares(std::mt19937& rng) {
    std::uniform_int_distribution<int> lots(1, 20);  // 100..2000 shares, round lots
    return static_cast<std::uint32_t>(lots(rng) * 100);
}

// Builds the wire bytes once so both book variants replay identical input.
// Each symbol keeps a pool of its currently-open orders so E/C/X/D/U
// messages reference real, live refs instead of missing ones — a lookup
// that always fails is a much cheaper (and unrepresentative) code path
// than the mutation being benchmarked.
std::vector<std::uint8_t> generate_synthetic_session() {
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

// ---------------------------------------------------------------------
// Per-book-type storage: OrderBook is default-constructible, LadderBook
// is not (it needs a price window up front), so lazy construction needs a
// hint price. Synthetic replay always has one (the symbol's known mid
// price); real-file replay falls back to the first add's own price, or a
// generic default if a locate is somehow seen without one first.
// ---------------------------------------------------------------------

template <typename BookType>
class BookStore;

template <>
class BookStore<book::OrderBook> {
public:
    book::OrderBook& get(std::uint16_t locate, std::uint32_t /*price_hint*/) {
        return books_[locate];
    }
    std::size_t book_count() const { return books_.size(); }

private:
    std::unordered_map<std::uint16_t, book::OrderBook> books_;
};

template <>
class BookStore<book::LadderBook> {
public:
    book::LadderBook& get(std::uint16_t locate, std::uint32_t price_hint) {
        auto it = books_.find(locate);
        if (it == books_.end()) {
            const std::uint32_t base = price_hint != 0 ? price_hint : kFallbackBase;
            it = books_.emplace(locate, book::LadderBook(base, kTickSize, kWindowPct)).first;
        }
        return it->second;
    }
    std::size_t book_count() const { return books_.size(); }

private:
    static constexpr std::uint32_t kFallbackBase = 500'000;  // $50.00, unseen-locate fallback
    static constexpr double kWindowPct = 0.30;
    std::unordered_map<std::uint16_t, book::LadderBook> books_;
};

using LatBuckets = std::array<std::vector<std::uint64_t>, 256>;

void merge_into(LatBuckets& dst, const LatBuckets& src) {
    for (std::size_t t = 0; t < dst.size(); ++t) {
        if (src[t].empty()) continue;
        dst[t].insert(dst[t].end(), src[t].begin(), src[t].end());
    }
}

// Times exactly the one call into the book per message — the map/array
// lookup that resolves which per-symbol book to hit happens outside the
// timed window, same for every message type and both book variants.
template <typename BookType>
struct BenchHandler {
    BookStore<BookType>& store;
    LatBuckets& lat;
    std::uint64_t unknown_refs = 0;

    static std::uint64_t ns_between(std::chrono::steady_clock::time_point a,
                                    std::chrono::steady_clock::time_point b) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
    }

    void on_add(const itch::AddOrder& m) {
        BookType& b = store.get(m.hdr.locate, m.price);
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = b.add(m.ref, m.side, m.shares, m.price);
        const auto t1 = std::chrono::steady_clock::now();
        lat['A'].push_back(ns_between(t0, t1));
        if (!ok) ++unknown_refs;
    }
    void on_execute(const itch::OrderExecuted& m) {
        BookType& b = store.get(m.hdr.locate, 0);
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = b.execute(m.ref, m.shares);
        const auto t1 = std::chrono::steady_clock::now();
        lat['E'].push_back(ns_between(t0, t1));
        if (!ok) ++unknown_refs;
    }
    void on_execute_price(const itch::OrderExecutedPrice& m) {
        BookType& b = store.get(m.hdr.locate, 0);
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = b.execute(m.ref, m.shares);
        const auto t1 = std::chrono::steady_clock::now();
        lat['C'].push_back(ns_between(t0, t1));
        if (!ok) ++unknown_refs;
    }
    void on_cancel(const itch::OrderCancel& m) {
        BookType& b = store.get(m.hdr.locate, 0);
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = b.cancel(m.ref, m.canceled);
        const auto t1 = std::chrono::steady_clock::now();
        lat['X'].push_back(ns_between(t0, t1));
        if (!ok) ++unknown_refs;
    }
    void on_delete(const itch::OrderDelete& m) {
        BookType& b = store.get(m.hdr.locate, 0);
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = b.remove(m.ref);
        const auto t1 = std::chrono::steady_clock::now();
        lat['D'].push_back(ns_between(t0, t1));
        if (!ok) ++unknown_refs;
    }
    void on_replace(const itch::OrderReplace& m) {
        BookType& b = store.get(m.hdr.locate, m.price);
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = b.replace(m.orig_ref, m.new_ref, m.shares, m.price);
        const auto t1 = std::chrono::steady_clock::now();
        lat['U'].push_back(ns_between(t0, t1));
        if (!ok) ++unknown_refs;
    }
    void on_other(char, std::size_t) {}
};

template <typename BookType>
struct PassResult {
    LatBuckets lat{};
    std::uint64_t unknown_refs = 0;
    std::size_t book_count = 0;
};

// One full replay against a FRESH set of books. Reusing a book across
// passes would replay duplicate refs into already-populated levels — a
// cheap early-return path, not the mutation this harness measures.
template <typename BookType>
PassResult<BookType> run_pass(const std::uint8_t* data, std::size_t len) {
    BookStore<BookType> store;
    PassResult<BookType> result;
    BenchHandler<BookType> h{store, result.lat};
    itch::parse_stream(data, len, h);
    result.unknown_refs = h.unknown_refs;
    result.book_count = store.book_count();
    return result;
}

// Same measurement, sourced from a .gz file re-read and re-inflated from
// disk for this one pass, instead of a buffer already resident in memory.
// This machine has 8GB RAM — decompressing a real multi-GB day once and
// keeping it around for repeated passes (the way the synthetic/mmap path
// does) isn't safe here, so each pass pays the decompression cost again in
// exchange for never needing the full day resident anywhere. GzipSource's
// own chunked inflate loop keeps this pass's memory footprint at a few MB
// regardless of file size, same as replay_main's --gz path.
template <typename BookType>
PassResult<BookType> run_pass_gz(const std::string& path) {
    BookStore<BookType> store;
    PassResult<BookType> result;
    BenchHandler<BookType> h{store, result.lat};
    io::GzipSource src(path);
    src.run(h);
    result.unknown_refs = h.unknown_refs;
    result.book_count = store.book_count();
    return result;
}

struct CsvRow {
    std::string book_type;
    std::string message_type;
    std::size_t count = 0;
    std::uint64_t p50 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t p999 = 0;
};

std::uint64_t percentile_sorted(const std::vector<std::uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    const std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

constexpr int kMeasuredPasses = 3;

void finalize_rows(const char* name, const LatBuckets& agg, std::vector<CsvRow>& rows) {
    for (char t : kMsgTypes) {
        const std::vector<std::uint64_t>& bucket = agg[static_cast<unsigned char>(t)];
        if (bucket.empty()) continue;
        std::vector<std::uint64_t> v = bucket;  // agg is shared read-only; sort a copy
        std::sort(v.begin(), v.end());
        CsvRow row;
        row.book_type = name;
        row.message_type = std::string(1, t);
        row.count = v.size();
        row.p50 = percentile_sorted(v, 0.50);
        row.p99 = percentile_sorted(v, 0.99);
        row.p999 = percentile_sorted(v, 0.999);
        rows.push_back(row);
    }
}

template <typename BookType>
void run_book_type(const char* name, const std::uint8_t* data, std::size_t len,
                   std::vector<CsvRow>& rows) {
    // Warm-up pass: page faults, allocator growth, cold caches all land
    // here and are discarded — this is what the README's "single run, no
    // warmup" caveat is asking for.
    { auto warm = run_pass<BookType>(data, len); (void)warm; }

    LatBuckets agg{};
    std::uint64_t total_unknown = 0;
    std::size_t book_count = 0;
    for (int p = 0; p < kMeasuredPasses; ++p) {
        auto r = run_pass<BookType>(data, len);
        merge_into(agg, r.lat);
        total_unknown += r.unknown_refs;
        book_count = r.book_count;
    }
    std::printf("[%s] warm-up + %d measured passes done (books=%zu, unknown_refs=%llu)\n", name,
               kMeasuredPasses, book_count, static_cast<unsigned long long>(total_unknown));
    finalize_rows(name, agg, rows);
}

// .gz variant: re-reads and re-inflates the file from disk for every pass
// (see run_pass_gz) instead of replaying a resident buffer. Slower per pass,
// bounded memory regardless of day size — the right tradeoff on an 8GB
// machine benchmarking a multi-GB real feed.
template <typename BookType>
void run_book_type_gz(const char* name, const std::string& path, std::vector<CsvRow>& rows) {
    { auto warm = run_pass_gz<BookType>(path); (void)warm; }

    LatBuckets agg{};
    std::uint64_t total_unknown = 0;
    std::size_t book_count = 0;
    for (int p = 0; p < kMeasuredPasses; ++p) {
        auto r = run_pass_gz<BookType>(path);
        merge_into(agg, r.lat);
        total_unknown += r.unknown_refs;
        book_count = r.book_count;
    }
    std::printf("[%s] warm-up + %d measured passes done (books=%zu, unknown_refs=%llu)\n", name,
               kMeasuredPasses, book_count, static_cast<unsigned long long>(total_unknown));
    finalize_rows(name, agg, rows);
}

void print_table(const std::vector<CsvRow>& rows) {
    std::printf("\n%-12s %-5s %12s %10s %10s %10s\n", "book_type", "type", "count", "p50_ns",
               "p99_ns", "p999_ns");
    for (const CsvRow& r : rows)
        std::printf("%-12s %-5s %12zu %10llu %10llu %10llu\n", r.book_type.c_str(),
                   r.message_type.c_str(), r.count, static_cast<unsigned long long>(r.p50),
                   static_cast<unsigned long long>(r.p99), static_cast<unsigned long long>(r.p999));
}

void write_csv(const std::vector<CsvRow>& rows, const std::string& path) {
    std::ofstream f(path);
    f << "book_type,message_type,count,p50_ns,p99_ns,p999_ns\n";
    for (const CsvRow& r : rows)
        f << r.book_type << ',' << r.message_type << ',' << r.count << ',' << r.p50 << ','
          << r.p99 << ',' << r.p999 << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::uint8_t> owned_buf;
    std::optional<io::MmapSource> mmap_src;
    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    bool synthetic = true;
    bool is_gz = false;
    std::string gz_path;

    if (argc >= 2) {
        const std::string path = argv[1];
        is_gz = path.size() >= 3 && path.compare(path.size() - 3, 3, ".gz") == 0;
        if (is_gz) {
            // Deliberately not mmap'd or buffered: a real full day can run
            // to multi-GB uncompressed, well past what fits alongside
            // everything else on an 8GB machine. Each pass streams+inflates
            // straight from the compressed file (see run_pass_gz) instead.
            gz_path = path;
            synthetic = false;
            std::printf("real data file (gzip-streamed, re-read per pass): %s\n", path.c_str());
        } else {
            try {
                mmap_src.emplace(path);
                data = mmap_src->data();
                len = mmap_src->size();
                synthetic = false;
                std::printf("real data file: %s (%zu bytes)\n", path.c_str(), len);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "warning: cannot open '%s' (%s); falling back to synthetic\n",
                            path.c_str(), e.what());
            }
        }
    }

    if (synthetic) {
        std::printf("generating synthetic session: %zu messages across %d symbols, seed=%u\n",
                   kTotalMessages, kNumSymbols, kSeed);
        owned_buf = generate_synthetic_session();
        data = owned_buf.data();
        len = owned_buf.size();
    }

    std::vector<CsvRow> rows;
    if (is_gz) {
        std::printf("gzip-streaming, %d warm-up + %d measured passes per book type\n\n", 1,
                   kMeasuredPasses);
        run_book_type_gz<book::OrderBook>("OrderBook", gz_path, rows);
        run_book_type_gz<book::LadderBook>("LadderBook", gz_path, rows);
    } else {
        std::printf("replaying %zu bytes, %d warm-up + %d measured passes per book type\n\n", len,
                   1, kMeasuredPasses);
        run_book_type<book::OrderBook>("OrderBook", data, len, rows);
        run_book_type<book::LadderBook>("LadderBook", data, len, rows);
    }

    print_table(rows);
    write_csv(rows, "bench/results.csv");
    std::printf("\nwrote bench/results.csv\n");
    return 0;
}
