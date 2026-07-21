// Benchmarks the two-thread replay pipeline (replay_threaded_main.cpp)
// against the single-threaded replay (replay_main.cpp) on byte-identical
// synthetic input, then isolates the SPSC handoff cost itself so the two
// numbers can be compared directly. See bench/THREADED_PIPELINE_FINDINGS.md
// for the resulting conclusion.
//
// replay_threaded_main.cpp is proven correct (tests/test_replay_threaded.cpp
// checks it produces identical book state to the single-threaded path) but
// was never measured for whether decoupling parsing from book-building onto
// two threads actually helps — or whether the cross-thread handoff (cache-
// line bouncing on the SPSC queue's atomics, spin-yield backpressure) costs
// more than the book mutations it's meant to overlap. bench/results.csv
// shows book mutations taking 40-500ns; if the SPSC round-trip costs more
// than that, decoupling is a net loss, not a win — this binary measures
// which is actually true on this machine.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "synthetic_session.hpp"
#include "itch/parser.hpp"
#include "pipeline/dispatch_to_book.hpp"
#include "pipeline/message.hpp"
#include "pipeline/spsc_queue.hpp"
#include "pipeline/threaded_replay.hpp"

namespace {

// Same capacity replay_threaded_main.cpp uses in production — the whole
// point is to measure that pipeline, not a differently-tuned one.
constexpr std::size_t kQueueCapacity = 1u << 16;
constexpr int kMeasuredPasses = 3;

// ---------------------------------------------------------------------
// Task 2: end-to-end wall-clock, single-threaded path vs threaded pipeline,
// same synthetic session, one discarded warm-up pass + 3 measured passes
// each (same discipline bench_main.cpp uses for book-mutation latency).
// ---------------------------------------------------------------------

struct PassTiming {
    std::size_t frames = 0;
    double elapsed_s = 0.0;
};

// Mirrors replay_main.cpp's run(): itch::parse_stream feeding a BookBuilder
// directly, one thread doing both parsing and book mutation inline.
PassTiming run_single_threaded_pass(const std::uint8_t* data, std::size_t len) {
    pipeline::BookBuilder<> h;  // defaults to book::LadderBook, same as run_threaded_pass below
    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t frames = itch::parse_stream(data, len, h);
    const auto t1 = std::chrono::steady_clock::now();
    return {frames, std::chrono::duration<double>(t1 - t0).count()};
}

// Mirrors replay_threaded_main.cpp's run(): the same input through the
// shared parser-thread/book-builder-thread pipeline (pipeline/threaded_
// replay.hpp), timed end to end including thread spawn/join — that overhead
// is part of what "decoupling costs" means for a wall-clock comparison.
PassTiming run_threaded_pass(const std::uint8_t* data, std::size_t len) {
    const auto t0 = std::chrono::steady_clock::now();
    auto stats = pipeline::run_pipeline<kQueueCapacity>(
        [&](pipeline::QueueProducer<kQueueCapacity>& p) { itch::parse_stream(data, len, p); });
    const auto t1 = std::chrono::steady_clock::now();
    return {stats.frames, std::chrono::duration<double>(t1 - t0).count()};
}

// Same as run_threaded_pass, except the parser thread is pinned to core 0
// and the book-builder thread to core 1 (see pipeline/thread_affinity.hpp).
// This is the control the original threaded-pipeline verdict was missing:
// without pinning, the OS scheduler can migrate either thread mid-run,
// which can look identical to "decoupling doesn't help" in the aggregate
// numbers while actually measuring migration noise instead of the design.
PassTiming run_threaded_pinned_pass(const std::uint8_t* data, std::size_t len) {
    const auto t0 = std::chrono::steady_clock::now();
    auto stats = pipeline::run_pipeline<kQueueCapacity>(
        [&](pipeline::QueueProducer<kQueueCapacity>& p) { itch::parse_stream(data, len, p); },
        pipeline::CoreAffinity{0, 1});
    const auto t1 = std::chrono::steady_clock::now();
    return {stats.frames, std::chrono::duration<double>(t1 - t0).count()};
}

struct PathResult {
    std::string name;
    std::size_t frames = 0;
    double median_elapsed_s = 0.0;
    double median_msgs_per_sec = 0.0;
    double best_msgs_per_sec = 0.0;  // fastest pass — see run_path for why this matters here
};

// Reports both the median and the best (fastest / least-perturbed) pass.
// This machine runs this benchmark alongside a large number of other
// concurrent background processes (observed load average ~7 on 8 cores),
// so individual passes see real, sometimes large scheduler-induced slowdowns
// that have nothing to do with the code under test — a process can only get
// slower from outside interference, never faster, so the fastest of a small
// number of passes is the standard way to recover a noise-robust estimate
// of true cost without needing hundreds of passes to average it out.
template <typename PassFn>
PathResult run_path(const char* name, PassFn&& pass_fn, const std::uint8_t* data,
                    std::size_t len) {
    { auto warm = pass_fn(data, len); (void)warm; }  // discarded: page faults, cold caches

    double elapsed[kMeasuredPasses];
    std::size_t frames = 0;
    for (int p = 0; p < kMeasuredPasses; ++p) {
        const PassTiming t = pass_fn(data, len);
        elapsed[p] = t.elapsed_s;
        frames = t.frames;
        std::printf("[%s] pass %d: %.3f s, %.0f msgs/s (frames=%zu)\n", name, p + 1, t.elapsed_s,
                    static_cast<double>(t.frames) / t.elapsed_s, t.frames);
    }

    double sorted[kMeasuredPasses];
    std::copy(elapsed, elapsed + kMeasuredPasses, sorted);
    std::sort(sorted, sorted + kMeasuredPasses);
    const double median_elapsed = sorted[kMeasuredPasses / 2];
    const double min_elapsed = sorted[0];

    PathResult result;
    result.name = name;
    result.frames = frames;
    result.median_elapsed_s = median_elapsed;
    result.median_msgs_per_sec = static_cast<double>(frames) / median_elapsed;
    result.best_msgs_per_sec = static_cast<double>(frames) / min_elapsed;
    return result;
}

// ---------------------------------------------------------------------
// Task 3: raw SPSC push()+pop() round-trip latency under real cross-thread
// contention — a producer thread and a consumer thread actually racing,
// not sequential same-thread push/pop (which would never see the cache-
// coherency cost this measurement exists to capture). Payload is one
// Envelope, matching what the production pipeline actually carries.
// ---------------------------------------------------------------------

// Envelope doesn't carry a sequence number of its own; AddOrder.ref is
// reused to carry one here purely so the consumer can look up this
// message's send timestamp — nothing in this measurement dispatches into a
// book, so the field's real meaning is irrelevant.
constexpr std::size_t kRoundtripMessages = bench::kTotalMessages;
using RoundtripQueue = pipeline::SpscQueue<pipeline::Envelope, kQueueCapacity>;

// Unthrottled: the producer pushes as fast as it can, exactly like
// QueueProducer's parser thread does in the real pipeline — it never
// rate-limits itself either. That matches actual pipeline behavior and
// upper-bounds the cost being measured, but it also means the bounded
// queue backs up (the parser thread can generate messages far faster than
// the book-builder thread can drain them — see Task 2's numbers), so most
// samples here are dominated by queueing delay behind that backlog, not by
// the push()/pop() mechanism itself. Useful for seeing what the real
// pipeline actually experiences under sustained load; not what isolates
// the handoff cost — that's measure_spsc_roundtrip_isolated below.
std::vector<std::uint64_t> measure_spsc_roundtrip_saturated() {
    RoundtripQueue queue;
    std::vector<std::chrono::steady_clock::time_point> send_time(kRoundtripMessages);
    std::vector<std::uint64_t> latency_ns(kRoundtripMessages);

    std::thread consumer([&] {
        pipeline::Envelope env;
        std::size_t received = 0;
        while (received < kRoundtripMessages) {
            if (!queue.pop(env)) {
                std::this_thread::yield();
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            const std::size_t seq = static_cast<std::size_t>(env.add.ref);
            latency_ns[seq] = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - send_time[seq])
                    .count());
            ++received;
        }
    });

    pipeline::Envelope env{};
    env.type = 'A';
    for (std::size_t i = 0; i < kRoundtripMessages; ++i) {
        send_time[i] = std::chrono::steady_clock::now();
        env.add.ref = i;
        while (!queue.push(env)) std::this_thread::yield();
    }
    consumer.join();

    return latency_ns;
}

// Paced: the producer waits for the queue to actually drain to empty
// before sending the next envelope, so every push lands in an empty queue
// and every round trip measures just the push-then-pop mechanism — the
// atomic store/load pair and the cache-line bounce between the two cores'
// caches — with no queueing delay mixed in. This is the number that's
// directly comparable to bench/results.csv's 40-500ns book-mutation costs,
// since it isolates the same kind of thing: per-message mechanical cost,
// not backlog.
std::vector<std::uint64_t> measure_spsc_roundtrip_isolated() {
    RoundtripQueue queue;
    std::vector<std::chrono::steady_clock::time_point> send_time(kRoundtripMessages);
    std::vector<std::uint64_t> latency_ns(kRoundtripMessages);

    std::thread consumer([&] {
        pipeline::Envelope env;
        std::size_t received = 0;
        while (received < kRoundtripMessages) {
            if (!queue.pop(env)) {
                std::this_thread::yield();
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            const std::size_t seq = static_cast<std::size_t>(env.add.ref);
            latency_ns[seq] = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - send_time[seq])
                    .count());
            ++received;
        }
    });

    pipeline::Envelope env{};
    env.type = 'A';
    for (std::size_t i = 0; i < kRoundtripMessages; ++i) {
        while (queue.size() != 0) std::this_thread::yield();
        send_time[i] = std::chrono::steady_clock::now();
        env.add.ref = i;
        while (!queue.push(env)) std::this_thread::yield();
    }
    consumer.join();

    return latency_ns;
}

std::uint64_t percentile_sorted(const std::vector<std::uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    const std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

// ---------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------

struct CsvRow {
    std::string metric;    // "throughput" or "spsc_roundtrip"
    std::string path;      // "single_threaded" / "threaded_pipeline" / "envelope_roundtrip"
    std::size_t count = 0;              // frames processed, or round-trip samples
    double elapsed_s = 0.0;             // throughput rows only: median wall-clock for the pass
    double messages_per_sec = 0.0;      // throughput rows only: median msgs/sec
    double best_msgs_per_sec = 0.0;     // throughput rows only: fastest of the 3 passes
    std::uint64_t p50_ns = 0;           // spsc_roundtrip row only: round-trip percentiles
    std::uint64_t p99_ns = 0;
    std::uint64_t p999_ns = 0;
    std::uint64_t min_ns = 0;           // spsc_roundtrip row only: fastest observed round trip
};

void write_csv(const std::vector<CsvRow>& rows, const std::string& path) {
    std::ofstream f(path);
    f << "metric,path,count,elapsed_s,messages_per_sec,best_msgs_per_sec,p50_ns,p99_ns,p999_ns,"
        "min_ns\n";
    for (const CsvRow& r : rows)
        f << r.metric << ',' << r.path << ',' << r.count << ',' << r.elapsed_s << ','
          << r.messages_per_sec << ',' << r.best_msgs_per_sec << ',' << r.p50_ns << ','
          << r.p99_ns << ',' << r.p999_ns << ',' << r.min_ns << '\n';
}

}  // namespace

int main() {
    std::printf("generating synthetic session: %zu messages across %d symbols, seed=%u\n",
               bench::kTotalMessages, bench::kNumSymbols, bench::kSeed);
    const std::vector<std::uint8_t> data = bench::generate_synthetic_session();
    std::printf("session: %zu bytes\n\n", data.size());

    std::vector<CsvRow> rows;

    std::printf("--- Task 2: single-threaded vs threaded pipeline (unpinned + pinned), %d "
               "warm-up + %d measured passes each ---\n",
               1, kMeasuredPasses);
    std::printf("core affinity mode: %s\n\n",
               pipeline::kHasHardAffinity
                   ? "hard pin (Linux pthread_setaffinity_np)"
                   : "best-effort scheduling hint only (macOS THREAD_AFFINITY_POLICY - "
                     "see README/thread_affinity.hpp for why this is not a real pin here)");
    const PathResult single =
        run_path("single_threaded", run_single_threaded_pass, data.data(), data.size());
    const PathResult threaded =
        run_path("threaded_pipeline", run_threaded_pass, data.data(), data.size());
    const PathResult pinned =
        run_path("threaded_pinned", run_threaded_pinned_pass, data.data(), data.size());

    const double speedup_median = threaded.median_msgs_per_sec / single.median_msgs_per_sec;
    const double speedup_best = threaded.best_msgs_per_sec / single.best_msgs_per_sec;
    const double pinned_speedup_median = pinned.median_msgs_per_sec / single.median_msgs_per_sec;
    const double pinned_speedup_best = pinned.best_msgs_per_sec / single.best_msgs_per_sec;
    std::printf("\nsingle_threaded    median %.3f s, %.0f msgs/s  (best pass %.0f msgs/s)\n",
               single.median_elapsed_s, single.median_msgs_per_sec, single.best_msgs_per_sec);
    std::printf("threaded_pipeline  median %.3f s, %.0f msgs/s  (best pass %.0f msgs/s)\n",
               threaded.median_elapsed_s, threaded.median_msgs_per_sec, threaded.best_msgs_per_sec);
    std::printf("threaded_pinned    median %.3f s, %.0f msgs/s  (best pass %.0f msgs/s)\n",
               pinned.median_elapsed_s, pinned.median_msgs_per_sec, pinned.best_msgs_per_sec);
    std::printf("speedup (unpinned threaded / single), median = %.3fx  %s\n", speedup_median,
               speedup_median >= 1.0 ? "(threaded faster)" : "(threaded SLOWER)");
    std::printf("speedup (unpinned threaded / single), best   = %.3fx  %s\n", speedup_best,
               speedup_best >= 1.0 ? "(threaded faster)" : "(threaded SLOWER)");
    std::printf("speedup (pinned threaded / single),   median = %.3fx  %s\n", pinned_speedup_median,
               pinned_speedup_median >= 1.0 ? "(pinned threaded faster)" : "(pinned threaded SLOWER)");
    std::printf("speedup (pinned threaded / single),   best   = %.3fx  %s\n\n", pinned_speedup_best,
               pinned_speedup_best >= 1.0 ? "(pinned threaded faster)" : "(pinned threaded SLOWER)");

    rows.push_back({"throughput", single.name, single.frames, single.median_elapsed_s,
                    single.median_msgs_per_sec, single.best_msgs_per_sec, 0, 0, 0, 0});
    rows.push_back({"throughput", threaded.name, threaded.frames, threaded.median_elapsed_s,
                    threaded.median_msgs_per_sec, threaded.best_msgs_per_sec, 0, 0, 0, 0});
    rows.push_back({"throughput", pinned.name, pinned.frames, pinned.median_elapsed_s,
                    pinned.median_msgs_per_sec, pinned.best_msgs_per_sec, 0, 0, 0, 0});

    std::printf("--- Task 3: SPSC push()+pop() round-trip under real cross-thread contention "
               "(%zu envelopes) ---\n",
               kRoundtripMessages);

    std::vector<std::uint64_t> saturated = measure_spsc_roundtrip_saturated();
    std::sort(saturated.begin(), saturated.end());
    const std::uint64_t sat_min = saturated.front();
    const std::uint64_t sat_p50 = percentile_sorted(saturated, 0.50);
    const std::uint64_t sat_p99 = percentile_sorted(saturated, 0.99);
    const std::uint64_t sat_p999 = percentile_sorted(saturated, 0.999);
    std::printf("spsc round-trip, saturated (producer never waits)\n");
    std::printf("    min=%llu ns  p50=%llu ns  p99=%llu ns  p999=%llu ns\n",
               static_cast<unsigned long long>(sat_min), static_cast<unsigned long long>(sat_p50),
               static_cast<unsigned long long>(sat_p99),
               static_cast<unsigned long long>(sat_p999));

    std::vector<std::uint64_t> isolated = measure_spsc_roundtrip_isolated();
    std::sort(isolated.begin(), isolated.end());
    const std::uint64_t iso_min = isolated.front();
    const std::uint64_t iso_p50 = percentile_sorted(isolated, 0.50);
    const std::uint64_t iso_p99 = percentile_sorted(isolated, 0.99);
    const std::uint64_t iso_p999 = percentile_sorted(isolated, 0.999);
    std::printf("spsc round-trip, isolated (producer waits for empty queue — pure handoff cost)\n");
    std::printf("    min=%llu ns  p50=%llu ns  p99=%llu ns  p999=%llu ns\n\n",
               static_cast<unsigned long long>(iso_min), static_cast<unsigned long long>(iso_p50),
               static_cast<unsigned long long>(iso_p99),
               static_cast<unsigned long long>(iso_p999));

    rows.push_back({"spsc_roundtrip", "envelope_roundtrip_saturated", saturated.size(), 0.0, 0.0,
                    0.0, sat_p50, sat_p99, sat_p999, sat_min});
    rows.push_back({"spsc_roundtrip", "envelope_roundtrip_isolated", isolated.size(), 0.0, 0.0,
                    0.0, iso_p50, iso_p99, iso_p999, iso_min});

    write_csv(rows, "bench/results_threaded.csv");
    std::printf("wrote bench/results_threaded.csv\n");
    return 0;
}
