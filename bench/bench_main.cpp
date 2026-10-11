// Head-to-head benchmark: OrderBook (std::map ladders) vs LadderBook (flat
// tick-indexed ladders) replaying the identical message stream. Absent a
// real NASDAQ file on the command line, the stream is a synthetic multi-
// symbol session generated in-process with a fixed RNG seed, so anyone who
// clones the repo can reproduce the run — no exchange data required.
//
// Methodology (see bench/README.md for the long form):
//
//   * Timing individual book calls with std::chrono::steady_clock is not
//     meaningful here. A single book mutation costs tens of nanoseconds, and
//     on Apple Silicon steady_clock is backed by the 24 MHz system counter —
//     one tick is ~41.7 ns, so a per-message measurement can only ever read
//     0, 41.7, 83.3, ... ns. The old harness's "42 ns p50" was exactly one
//     tick, not a latency.
//
//   * Primary measurement: BATCHES. The stream is decoded once into a
//     vector of pipeline::Envelope (the same POD the threaded pipeline
//     carries), then replayed through the production BookBuilder /
//     dispatch_to_book path. The clock is read once per batch of
//     kDefaultBatch (1024) messages, so one clock tick is a tiny fraction of
//     each measured interval. From that we report:
//       - throughput (messages / second = total messages / total timed ns),
//       - per-message amortized cost percentiles: the distribution over
//         batches of (batch ns / batch size). This is NOT the tail latency
//         of an individual message — a single slow message is averaged
//         with 1023 others — and the output says so.
//
//   * Optional per-message, per-type numbers: only where a fine-grained
//     cycle counter exists (x86-64 rdtsc, lfence-serialized, calibrated
//     against steady_clock, with the measured empty-timer overhead
//     subtracted). On arm64 the user-visible counter (cntvct_el0) runs at
//     the same 24 MHz as steady_clock on Apple M-series, so per-type rows are
//     skipped there rather than published as quantized numbers.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#if defined(__x86_64__)
#include <x86intrin.h>
#define ITCH_BENCH_HAVE_TSC 1
#else
#define ITCH_BENCH_HAVE_TSC 0
#endif

#include "synthetic_session.hpp"
#include "book/ladder_book.hpp"
#include "book/order_book.hpp"
#include "io/gzip_source.hpp"
#include "io/mmap_source.hpp"
#include "itch/parser.hpp"
#include "pipeline/dispatch_to_book.hpp"
#include "pipeline/message.hpp"

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kDefaultBatch = 1024;
constexpr int kDefaultPasses = 3;

std::uint64_t ns_between(Clock::time_point a, Clock::time_point b) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

// ---------------------------------------------------------------- timer info

struct TimerInfo {
    double resolution_ns = 0;  // smallest observed nonzero steady_clock step
    double read_cost_ns = 0;   // mean cost of one steady_clock::now()
};

TimerInfo probe_steady_clock() {
    TimerInfo info;
    // Resolution: spin until the clock visibly advances, many times, keep
    // the smallest step. On a ~1 ns clock this is roughly the read cost; on
    // a 24 MHz counter it is ~41.7 ns.
    std::uint64_t best = UINT64_MAX;
    for (int i = 0; i < 2000; ++i) {
        const auto a = Clock::now();
        auto b = Clock::now();
        while (b == a) b = Clock::now();
        best = std::min(best, ns_between(a, b));
    }
    info.resolution_ns = static_cast<double>(best);

    constexpr int kReads = 1'000'000;
    const auto t0 = Clock::now();
    Clock::time_point sink{};
    for (int i = 0; i < kReads; ++i) sink = std::max(sink, Clock::now());
    const auto t1 = Clock::now();
    info.read_cost_ns = static_cast<double>(ns_between(t0, t1)) / kReads;
    (void)sink;
    return info;
}

// ----------------------------------------------------- decoded message stream

struct Decoder {
    std::vector<pipeline::Envelope>& out;
    void on_add(const itch::AddOrder& m) { out.push_back(pipeline::Envelope{.type = 'A', .add = m}); }
    void on_execute(const itch::OrderExecuted& m) {
        out.push_back(pipeline::Envelope{.type = 'E', .exec = m});
    }
    void on_execute_price(const itch::OrderExecutedPrice& m) {
        out.push_back(pipeline::Envelope{.type = 'C', .exec_price = m});
    }
    void on_cancel(const itch::OrderCancel& m) {
        out.push_back(pipeline::Envelope{.type = 'X', .cancel = m});
    }
    void on_delete(const itch::OrderDelete& m) { out.push_back(pipeline::Envelope{.type = 'D', .del = m}); }
    void on_replace(const itch::OrderReplace& m) {
        out.push_back(pipeline::Envelope{.type = 'U', .replace = m});
    }
    void on_other(char, std::size_t) {}  // not book-mutating: not part of what's timed
};

// ------------------------------------------------------------ batch timing

struct BatchStats {
    std::vector<double> per_msg_ns;  // one entry per full batch: batch_ns / batch_len
    std::uint64_t messages = 0;
    std::uint64_t timed_ns = 0;
    std::uint64_t unknown_refs = 0;
    std::size_t book_count = 0;
};

// One full replay against a FRESH set of books (reusing books across passes
// would replay duplicate refs into populated levels — a cheap early-return
// path, not the mutation being measured).
template <typename BookType>
void run_batched_pass(const std::vector<pipeline::Envelope>& msgs, std::size_t batch,
                      BatchStats& stats) {
    pipeline::BookBuilder<BookType> builder;
    const std::size_t n = msgs.size();
    for (std::size_t start = 0; start < n; start += batch) {
        const std::size_t end = std::min(n, start + batch);
        const auto t0 = Clock::now();
        for (std::size_t i = start; i < end; ++i) pipeline::dispatch_to_book(msgs[i], builder);
        const auto t1 = Clock::now();
        const std::uint64_t ns = ns_between(t0, t1);
        stats.timed_ns += ns;
        stats.messages += end - start;
        // A short trailing batch still counts toward throughput, but its
        // per-message cost would be dominated by clock granularity.
        if (end - start == batch)
            stats.per_msg_ns.push_back(static_cast<double>(ns) / static_cast<double>(batch));
    }
    stats.unknown_refs += builder.unknown_refs;
    stats.book_count = builder.books.book_count();
}

// .gz variant: the day is streamed and inflated per pass instead of being
// decoded into memory first (a full real day does not fit in 8 GB as
// Envelopes). Each timed batch therefore includes parse + inflate cost, and
// the CSV's method column says so.
template <typename BookType>
struct GzBatchHandler {
    pipeline::BookBuilder<BookType>& builder;
    BatchStats& stats;
    std::size_t batch;
    std::size_t in_batch = 0;
    Clock::time_point t0 = Clock::now();

    void tick() {
        if (++in_batch < batch) return;
        const auto t1 = Clock::now();
        const std::uint64_t ns = ns_between(t0, t1);
        stats.timed_ns += ns;
        stats.messages += in_batch;
        stats.per_msg_ns.push_back(static_cast<double>(ns) / static_cast<double>(in_batch));
        in_batch = 0;
        t0 = Clock::now();
    }
    void on_add(const itch::AddOrder& m) { builder.on_add(m); tick(); }
    void on_execute(const itch::OrderExecuted& m) { builder.on_execute(m); tick(); }
    void on_execute_price(const itch::OrderExecutedPrice& m) { builder.on_execute_price(m); tick(); }
    void on_cancel(const itch::OrderCancel& m) { builder.on_cancel(m); tick(); }
    void on_delete(const itch::OrderDelete& m) { builder.on_delete(m); tick(); }
    void on_replace(const itch::OrderReplace& m) { builder.on_replace(m); tick(); }
    void on_other(char t, std::size_t n) { builder.on_other(t, n); }
};

template <typename BookType>
void run_batched_pass_gz(const std::string& path, std::size_t batch, BatchStats& stats) {
    pipeline::BookBuilder<BookType> builder;
    GzBatchHandler<BookType> h{builder, stats, batch};
    io::GzipSource src(path);
    h.t0 = Clock::now();
    src.run(h);
    stats.unknown_refs += builder.unknown_refs;
    stats.book_count = builder.books.book_count();
}

// ------------------------------------------------- per-type cycle counting

#if ITCH_BENCH_HAVE_TSC
inline std::uint64_t tsc_begin() {
    _mm_lfence();
    return __rdtsc();
}
inline std::uint64_t tsc_end() {
    unsigned aux = 0;
    const std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

struct TscCalibration {
    double ns_per_tick = 0;
    double overhead_ticks = 0;  // median cost of an empty begin/end pair
};

TscCalibration calibrate_tsc() {
    TscCalibration c;
    const auto w0 = Clock::now();
    const std::uint64_t c0 = tsc_begin();
    while (ns_between(w0, Clock::now()) < 200'000'000) {
    }
    const std::uint64_t c1 = tsc_end();
    const auto w1 = Clock::now();
    c.ns_per_tick = static_cast<double>(ns_between(w0, w1)) / static_cast<double>(c1 - c0);

    std::vector<std::uint64_t> empty(200'000);
    for (auto& e : empty) {
        const std::uint64_t a = tsc_begin();
        const std::uint64_t b = tsc_end();
        e = b - a;
    }
    std::sort(empty.begin(), empty.end());
    c.overhead_ticks = static_cast<double>(empty[empty.size() / 2]);
    return c;
}

using TypeBuckets = std::array<std::vector<double>, 256>;

template <typename BookType>
void run_per_type_pass(const std::vector<pipeline::Envelope>& msgs, const TscCalibration& cal,
                       TypeBuckets& out) {
    pipeline::BookBuilder<BookType> builder;
    for (const pipeline::Envelope& env : msgs) {
        const std::uint64_t a = tsc_begin();
        pipeline::dispatch_to_book(env, builder);
        const std::uint64_t b = tsc_end();
        const double ticks = std::max(0.0, static_cast<double>(b - a) - cal.overhead_ticks);
        out[static_cast<unsigned char>(env.type)].push_back(ticks * cal.ns_per_tick);
    }
}
#endif

// ------------------------------------------------------------------ results

struct Row {
    std::string book_type;
    std::string message_type;  // "ALL" for batch rows, else an ITCH type code
    std::size_t count = 0;     // full batches for ALL rows, messages for per-type rows
    double p50 = 0, p99 = 0, p999 = 0;
    double msgs_per_sec = 0;   // ALL rows only
    std::string method;
};

double percentile_sorted(const std::vector<double>& v, double p) {
    if (v.empty()) return 0;
    return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))];
}

Row batch_row(const char* name, BatchStats s, std::size_t batch, bool gz) {
    std::sort(s.per_msg_ns.begin(), s.per_msg_ns.end());
    Row r;
    r.book_type = name;
    r.message_type = "ALL";
    r.count = s.per_msg_ns.size();
    r.p50 = percentile_sorted(s.per_msg_ns, 0.50);
    r.p99 = percentile_sorted(s.per_msg_ns, 0.99);
    r.p999 = percentile_sorted(s.per_msg_ns, 0.999);
    r.msgs_per_sec = s.timed_ns ? static_cast<double>(s.messages) * 1e9 /
                                      static_cast<double>(s.timed_ns)
                                : 0;
    r.method = "batch" + std::to_string(batch) + (gz ? "_incl_parse_inflate" : "");
    return r;
}

struct Options {
    std::string input;
    std::size_t batch = kDefaultBatch;
    int passes = kDefaultPasses;
    std::string csv_out = "bench/results.csv";
    std::string summary_md;  // append a markdown table here if set
    bool per_type = true;
};

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&](const char* flag) -> std::optional<std::string> {
            const std::string f = std::string(flag) + "=";
            if (a.rfind(f, 0) == 0) return a.substr(f.size());
            return std::nullopt;
        };
        if (auto v = val("--batch")) {
            o.batch = static_cast<std::size_t>(std::strtoull(v->c_str(), nullptr, 10));
        } else if (auto p = val("--passes")) {
            o.passes = std::atoi(p->c_str());
        } else if (auto out = val("--out")) {
            o.csv_out = *out;
        } else if (auto md = val("--summary-md")) {
            o.summary_md = *md;
        } else if (a == "--no-per-type") {
            o.per_type = false;
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: bench [FILE|FILE.gz] [--batch=N] [--passes=N] [--out=CSV]\n"
                "             [--summary-md=PATH] [--no-per-type]\n");
            std::exit(0);
        } else {
            o.input = a;
        }
    }
    if (o.batch == 0) o.batch = kDefaultBatch;
    if (o.passes <= 0) o.passes = kDefaultPasses;
    return o;
}

template <typename BookType>
void bench_book(const char* name, const Options& opt, const std::vector<pipeline::Envelope>* msgs,
                std::vector<Row>& rows) {
    const bool gz = msgs == nullptr;
    {
        BatchStats warm;  // page faults, allocator growth, cold caches land here
        if (gz) run_batched_pass_gz<BookType>(opt.input, opt.batch, warm);
        else run_batched_pass<BookType>(*msgs, opt.batch, warm);
    }
    BatchStats agg;
    for (int p = 0; p < opt.passes; ++p) {
        if (gz) run_batched_pass_gz<BookType>(opt.input, opt.batch, agg);
        else run_batched_pass<BookType>(*msgs, opt.batch, agg);
    }
    std::printf("[%s] warm-up + %d measured passes (books=%zu, unknown_refs=%llu)\n", name,
                opt.passes, agg.book_count, static_cast<unsigned long long>(agg.unknown_refs));
    rows.push_back(batch_row(name, std::move(agg), opt.batch, gz));
}

#if ITCH_BENCH_HAVE_TSC
template <typename BookType>
void bench_book_per_type(const char* name, const Options& opt,
                         const std::vector<pipeline::Envelope>& msgs, const TscCalibration& cal,
                         std::vector<Row>& rows) {
    {
        TypeBuckets warm;
        run_per_type_pass<BookType>(msgs, cal, warm);
    }
    TypeBuckets agg;
    for (int p = 0; p < opt.passes; ++p) run_per_type_pass<BookType>(msgs, cal, agg);
    for (char t : bench::kMsgTypes) {
        std::vector<double>& v = agg[static_cast<unsigned char>(t)];
        if (v.empty()) continue;
        std::sort(v.begin(), v.end());
        Row r;
        r.book_type = name;
        r.message_type = std::string(1, t);
        r.count = v.size();
        r.p50 = percentile_sorted(v, 0.50);
        r.p99 = percentile_sorted(v, 0.99);
        r.p999 = percentile_sorted(v, 0.999);
        r.method = "rdtsc_per_msg";
        rows.push_back(r);
    }
}
#endif

void print_rows(const std::vector<Row>& rows) {
    std::printf("\n%-11s %-4s %10s %11s %11s %11s %14s  %s\n", "book_type", "type", "count",
                "p50_ns", "p99_ns", "p999_ns", "msgs_per_sec", "method");
    for (const Row& r : rows)
        std::printf("%-11s %-4s %10zu %11.1f %11.1f %11.1f %14.0f  %s\n", r.book_type.c_str(),
                    r.message_type.c_str(), r.count, r.p50, r.p99, r.p999, r.msgs_per_sec,
                    r.method.c_str());
}

void write_csv(const std::vector<Row>& rows, const std::string& path) {
    std::ofstream f(path);
    f << "book_type,message_type,count,p50_ns,p99_ns,p999_ns,msgs_per_sec,method\n";
    char buf[256];
    for (const Row& r : rows) {
        std::snprintf(buf, sizeof buf, "%s,%s,%zu,%.1f,%.1f,%.1f,%.0f,%s\n", r.book_type.c_str(),
                      r.message_type.c_str(), r.count, r.p50, r.p99, r.p999, r.msgs_per_sec,
                      r.method.c_str());
        f << buf;
    }
}

void write_summary_md(const std::vector<Row>& rows, const std::string& path,
                      const TimerInfo& timer, const Options& opt, std::size_t messages) {
    std::ofstream f(path, std::ios::app);
    char buf[512];
    f << "### OrderBook vs LadderBook (`./build/bench`)\n\n";
    std::snprintf(buf, sizeof buf,
                  "Book messages per pass: %zu · batch size: %zu · measured passes: %d (after "
                  "one discarded warm-up) · steady_clock resolution observed: %.1f ns, read "
                  "cost %.1f ns\n\n",
                  messages, opt.batch, opt.passes, timer.resolution_ns, timer.read_cost_ns);
    f << buf;
    f << "| book | type | count | p50 ns/msg | p99 ns/msg | p99.9 ns/msg | M msg/s | method |\n";
    f << "|---|---|---:|---:|---:|---:|---:|---|\n";
    for (const Row& r : rows) {
        char mps[32] = "-";
        if (r.msgs_per_sec > 0) std::snprintf(mps, sizeof mps, "%.2f", r.msgs_per_sec / 1e6);
        std::snprintf(buf, sizeof buf, "| %s | %s | %zu | %.1f | %.1f | %.1f | %s | %s |\n",
                      r.book_type.c_str(), r.message_type.c_str(), r.count, r.p50, r.p99, r.p999,
                      mps, r.method.c_str());
        f << buf;
    }
    f << "\n`ALL` rows: per-message cost amortized over each batch (the distribution is over "
         "batches, not individual messages). Per-type rows (x86-64 only): one rdtsc pair per "
         "call, median empty-timer overhead subtracted.\n\n";
}

}  // namespace

int main(int argc, char** argv) {
    const Options opt = parse_args(argc, argv);

    const TimerInfo timer = probe_steady_clock();
    std::printf("steady_clock: observed resolution %.1f ns, read cost %.1f ns\n",
                timer.resolution_ns, timer.read_cost_ns);

    const bool is_gz =
        opt.input.size() >= 3 && opt.input.compare(opt.input.size() - 3, 3, ".gz") == 0;

    std::vector<pipeline::Envelope> msgs;
    std::vector<Row> rows;

    if (is_gz) {
        // A real full day can run to multi-GB uncompressed — each pass
        // streams + inflates from the compressed file instead of holding the
        // day in memory, so batch timings include parse + inflate.
        std::printf("real data file (gzip-streamed, re-read per pass): %s\n", opt.input.c_str());
        bench_book<book::OrderBook>("OrderBook", opt, nullptr, rows);
        bench_book<book::LadderBook>("LadderBook", opt, nullptr, rows);
    } else {
        std::vector<std::uint8_t> owned;
        std::optional<io::MmapSource> mm;
        const std::uint8_t* data = nullptr;
        std::size_t len = 0;
        if (!opt.input.empty()) {
            try {
                mm.emplace(opt.input);
                data = mm->data();
                len = mm->size();
                std::printf("real data file: %s (%zu bytes)\n", opt.input.c_str(), len);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "warning: cannot open '%s' (%s); using synthetic\n",
                             opt.input.c_str(), e.what());
            }
        }
        if (data == nullptr) {
            std::printf("synthetic session: %zu messages across %d symbols, seed=%u\n",
                        bench::kTotalMessages, bench::kNumSymbols, bench::kSeed);
            owned = bench::generate_synthetic_session();
            data = owned.data();
            len = owned.size();
        }
        Decoder dec{msgs};
        itch::parse_stream(data, len, dec);
        std::printf("decoded %zu book messages; batch=%zu, 1 warm-up + %d measured passes per "
                    "book\n\n",
                    msgs.size(), opt.batch, opt.passes);

        bench_book<book::OrderBook>("OrderBook", opt, &msgs, rows);
        bench_book<book::LadderBook>("LadderBook", opt, &msgs, rows);

#if ITCH_BENCH_HAVE_TSC
        if (opt.per_type) {
            const TscCalibration cal = calibrate_tsc();
            std::printf("rdtsc: %.4f ns/tick, empty-timer overhead %.0f ticks (subtracted)\n",
                        cal.ns_per_tick, cal.overhead_ticks);
            bench_book_per_type<book::OrderBook>("OrderBook", opt, msgs, cal, rows);
            bench_book_per_type<book::LadderBook>("LadderBook", opt, msgs, cal, rows);
        }
#else
        if (opt.per_type)
            std::printf("per-message, per-type timing skipped: no sub-tick cycle counter on this "
                        "platform (steady_clock resolution %.1f ns)\n",
                        timer.resolution_ns);
#endif
    }

    print_rows(rows);
    write_csv(rows, opt.csv_out);
    std::printf("\nwrote %s\n", opt.csv_out.c_str());
    if (!opt.summary_md.empty()) write_summary_md(rows, opt.summary_md, timer, opt, msgs.size());
    return 0;
}
