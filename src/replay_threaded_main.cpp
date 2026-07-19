#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "io/gzip_source.hpp"
#include "io/mmap_source.hpp"
#include "itch/encode.hpp"
#include "itch/parser.hpp"
#include "pipeline/dispatch_to_book.hpp"
#include "pipeline/message.hpp"
#include "pipeline/spsc_queue.hpp"

// Alternative to replay_main: the same ITCH replay, decoupled into a parser
// thread and a book-builder thread joined by a lock-free SPSC queue, instead
// of one thread doing both inline. This is a comparison binary, not a
// replacement — replay_main.cpp is untouched.
namespace {

using pipeline::BookBuilder;
using pipeline::Envelope;

// Arbitrary but generous: large enough that ordinary parse-vs-book-mutation
// speed differences rarely fill it, so max-occupancy numbers reported below
// mean something (a queue that's *always* full just measures its own size).
constexpr std::size_t kQueueCapacity = 1u << 16;
using Queue = pipeline::SpscQueue<Envelope, kQueueCapacity>;

// Parser-thread handler: matches the itch::dispatch Handler interface
// exactly (same as BookBuilder does for the single-threaded path), but
// instead of mutating a book inline, it packages each decoded message into
// an Envelope and hands it to the book-builder thread over the queue. Since
// the interface matches, this drops straight into itch::parse_stream /
// GzipSource::run without any parser-side changes.
struct QueueProducer {
    Queue& queue;
    std::size_t max_occupancy = 0;
    std::size_t frames = 0;

    // Deliberate simple backpressure: if the queue is full, the book-builder
    // thread is behind, so spin-yield and retry rather than drop the
    // message or block on a condvar. A production system would size the
    // queue from measured backpressure or apply real flow control; that
    // adaptiveness is out of scope here — the point of this binary is to
    // *measure* how often backpressure happens (max_occupancy in the
    // report), not to eliminate it.
    void push_spin(const Envelope& env) {
        while (!queue.push(env)) std::this_thread::yield();
        const std::size_t occ = queue.size();
        if (occ > max_occupancy) max_occupancy = occ;
        ++frames;
    }

    void on_add(const itch::AddOrder& m) { push_spin(Envelope{.type = 'A', .add = m}); }
    void on_execute(const itch::OrderExecuted& m) {
        push_spin(Envelope{.type = 'E', .exec = m});
    }
    void on_execute_price(const itch::OrderExecutedPrice& m) {
        push_spin(Envelope{.type = 'C', .exec_price = m});
    }
    void on_cancel(const itch::OrderCancel& m) { push_spin(Envelope{.type = 'X', .cancel = m}); }
    void on_delete(const itch::OrderDelete& m) { push_spin(Envelope{.type = 'D', .del = m}); }
    void on_replace(const itch::OrderReplace& m) {
        push_spin(Envelope{.type = 'U', .replace = m});
    }
    // Skipped/corrupt frames don't touch any book, but still get pushed
    // (bare tag, no payload) so the consumer's counts (frames-by-type in the
    // report) stay a complete picture, owned by one thread, rather than
    // splitting bookkeeping across both.
    void on_other(char type, std::size_t) { push_spin(Envelope{.type = type}); }
};

struct ConsumerResult {
    BookBuilder builder;
    std::size_t frames = 0;
};

// Book-builder thread body: pops envelopes and re-dispatches them into the
// book via the same BookBuilder on_* methods replay_main.cpp's single
// thread calls directly. producer_done is checked only after a pop comes up
// empty, and once it's observed true the queue is drained completely before
// exiting — the producer can't push anything more after setting it, so a
// full drain at that point is guaranteed to be the true end of the stream.
void consume(Queue& queue, const std::atomic<bool>& producer_done, ConsumerResult& result) {
    Envelope env;
    while (true) {
        if (queue.pop(env)) {
            pipeline::dispatch_to_book(env, result.builder);
            ++result.frames;
            continue;
        }
        if (producer_done.load(std::memory_order_acquire)) {
            while (queue.pop(env)) {
                pipeline::dispatch_to_book(env, result.builder);
                ++result.frames;
            }
            break;
        }
        std::this_thread::yield();
    }
}

struct RunStats {
    BookBuilder builder;
    std::size_t frames = 0;
    std::size_t max_occupancy = 0;
};

// Runs `decode` (a callable that walks the input and feeds a QueueProducer)
// on a dedicated parser thread while a book-builder thread drains the queue
// concurrently, then joins both. Every RunStats field below is written by
// exactly one of the two threads and only read here after both threads have
// joined, so no additional synchronization is needed beyond the join()s and
// the producer_done flag consume() already uses to know when to stop.
template <typename Decode>
RunStats run_pipeline(Decode&& decode) {
    Queue queue;
    std::atomic<bool> producer_done{false};
    QueueProducer producer{queue};
    ConsumerResult consumer_result;

    std::thread book_builder_thread(consume, std::ref(queue), std::cref(producer_done),
                                     std::ref(consumer_result));
    std::thread parser_thread([&] {
        decode(producer);
        producer_done.store(true, std::memory_order_release);
    });

    parser_thread.join();
    book_builder_thread.join();

    return RunStats{std::move(consumer_result.builder), consumer_result.frames,
                     producer.max_occupancy};
}

void report(const BookBuilder& h, std::size_t bytes, std::size_t frames, double secs,
            std::size_t max_occupancy) {
    std::size_t open_orders = 0, bid_levels = 0, ask_levels = 0;
    for (const auto& [locate, b] : h.books) {
        open_orders += b.open_orders();
        bid_levels += b.bid_levels();
        ask_levels += b.ask_levels();
    }

    std::printf("bytes            %zu\n", bytes);
    std::printf("frames           %zu\n", frames);
    std::printf("elapsed          %.3f s\n", secs);
    if (secs > 0.0)
        std::printf("throughput       %.0f frames/s (single run, no warmup — see bench target)\n",
                    static_cast<double>(frames) / secs);
    std::printf("books            %zu\n", h.books.size());
    std::printf("open orders      %zu (bid levels %zu, ask levels %zu)\n",
                open_orders, bid_levels, ask_levels);
    std::printf("unknown refs     %llu\n",
                static_cast<unsigned long long>(h.unknown_refs));
    std::printf("frames by type   ");
    for (int t = 0; t < 256; ++t)
        if (h.counts[static_cast<std::size_t>(t)] > 0)
            std::printf("%c=%llu ", static_cast<char>(t),
                        static_cast<unsigned long long>(h.counts[static_cast<std::size_t>(t)]));
    std::printf("\n");
    std::printf("max queue occ    %zu / %zu (%.1f%% of capacity — backpressure indicator)\n",
                max_occupancy, kQueueCapacity,
                100.0 * static_cast<double>(max_occupancy) / static_cast<double>(kQueueCapacity));
}

int run(const std::uint8_t* data, std::size_t len) {
    const auto t0 = std::chrono::steady_clock::now();
    RunStats stats = run_pipeline([&](QueueProducer& p) { itch::parse_stream(data, len, p); });
    const auto t1 = std::chrono::steady_clock::now();
    report(stats.builder, len, stats.frames, std::chrono::duration<double>(t1 - t0).count(),
           stats.max_occupancy);
    return 0;
}

int run_mmap(const std::string& path) {
    io::MmapSource src(path);
    const auto t0 = std::chrono::steady_clock::now();
    RunStats stats =
        run_pipeline([&](QueueProducer& p) { itch::parse_stream(src.data(), src.size(), p); });
    const auto t1 = std::chrono::steady_clock::now();
    report(stats.builder, src.size(), stats.frames,
           std::chrono::duration<double>(t1 - t0).count(), stats.max_occupancy);
    return 0;
}

int run_gzip(const std::string& path) {
    io::GzipSource src(path);
    const auto t0 = std::chrono::steady_clock::now();
    RunStats stats = run_pipeline([&](QueueProducer& p) { src.run(p); });
    const auto t1 = std::chrono::steady_clock::now();
    report(stats.builder, src.bytes_decompressed(), stats.frames,
           std::chrono::duration<double>(t1 - t0).count(), stats.max_occupancy);
    return 0;
}

int run_legacy(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::fprintf(stderr, "error: cannot open %s\n", path.c_str());
        return 1;
    }
    const std::streamsize size = f.tellg();
    f.seekg(0);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    if (!f.read(reinterpret_cast<char*>(buf.data()), size)) {
        std::fprintf(stderr, "error: short read on %s\n", path.c_str());
        return 1;
    }
    return run(buf.data(), buf.size());
}

// Same synthetic session as replay_main.cpp's selftest(), so the two
// binaries are directly comparable on identical input.
int selftest() {
    using namespace itch::encode;
    Msg system_event;
    header(system_event, 'S', 0, 1);
    system_event.push_back('O');

    const std::vector<std::uint8_t> buf = stream({
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
    return run(buf.data(), buf.size());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0) return selftest();
    if (argc == 3 && std::strcmp(argv[1], "--legacy") == 0) return run_legacy(argv[2]);
    if (argc != 2) {
        std::fprintf(stderr,
                     "usage: %s <file>           mmap (or gz-stream, if the name ends in .gz)\n"
                     "                            and replay a NASDAQ ITCH 5.0 file through the\n"
                     "                            parser-thread/book-builder-thread pipeline\n"
                     "       %s --legacy <file>   read the whole file into memory first, then replay\n"
                     "       %s --selftest        replay a built-in synthetic session\n",
                     argv[0], argv[0], argv[0]);
        return 2;
    }

    const std::string path = argv[1];
    const bool is_gz = path.size() >= 3 && path.compare(path.size() - 3, 3, ".gz") == 0;
    try {
        return is_gz ? run_gzip(path) : run_mmap(path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
