#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <string>
#include <vector>

#include "book/ladder_book.hpp"
#include "book/order_book.hpp"
#include "io/gzip_source.hpp"
#include "io/mmap_source.hpp"
#include "itch/encode.hpp"
#include "itch/parser.hpp"
#include "pipeline/threaded_replay.hpp"

// Alternative to replay_main: the same ITCH replay, decoupled into a parser
// thread and a book-builder thread joined by a lock-free SPSC queue, instead
// of one thread doing both inline. This is a comparison binary, not a
// replacement — replay_main.cpp is untouched. The pipeline machinery itself
// (QueueProducer/consume/run_pipeline) lives in
// include/pipeline/threaded_replay.hpp, shared with
// bench/bench_threaded_main.cpp.
namespace {

using pipeline::BookBuilder;
using pipeline::QueueProducer;
using pipeline::RunStats;

// Arbitrary but generous: large enough that ordinary parse-vs-book-mutation
// speed differences rarely fill it, so max-occupancy numbers reported below
// mean something (a queue that's *always* full just measures its own size).
constexpr std::size_t kQueueCapacity = 1u << 16;

// --pin's fixed core assignment: parser on core 0, book-builder on core 1.
// See pipeline/thread_affinity.hpp for what "pinned" means per platform.
pipeline::CoreAffinity affinity_for(bool pin) {
    if (!pin) return {};
    return pipeline::CoreAffinity{0, 1};
}

template <typename BookType>
void report(const BookBuilder<BookType>& h, std::size_t bytes, std::size_t frames, double secs,
            std::size_t max_occupancy, bool pin) {
    std::size_t open_orders = 0, bid_levels = 0, ask_levels = 0;
    h.books.for_each([&](std::uint16_t, const BookType& b) {
        open_orders += b.open_orders();
        bid_levels += b.bid_levels();
        ask_levels += b.ask_levels();
    });

    std::printf("bytes            %zu\n", bytes);
    std::printf("frames           %zu\n", frames);
    std::printf("elapsed          %.3f s\n", secs);
    if (secs > 0.0)
        std::printf("throughput       %.0f frames/s (single run, no warmup — see bench target)\n",
                    static_cast<double>(frames) / secs);
    std::printf("books            %zu\n", h.books.book_count());
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
    if (pin) {
        std::printf("core affinity    parser=core0 book_builder=core1 (%s)\n",
                    pipeline::kHasHardAffinity ? "hard pin" : "best-effort hint only, see README");
    }
}

template <typename BookType>
int run(const std::uint8_t* data, std::size_t len, bool pin) {
    const auto t0 = std::chrono::steady_clock::now();
    RunStats<BookType> stats = pipeline::run_pipeline<kQueueCapacity, BookType>(
        [&](QueueProducer<kQueueCapacity>& p) { itch::parse_stream(data, len, p); },
        affinity_for(pin));
    const auto t1 = std::chrono::steady_clock::now();
    report(stats.builder, len, stats.frames, std::chrono::duration<double>(t1 - t0).count(),
           stats.max_occupancy, pin);
    return 0;
}

template <typename BookType>
int run_mmap(const std::string& path, bool pin) {
    io::MmapSource src(path);
    const auto t0 = std::chrono::steady_clock::now();
    RunStats<BookType> stats = pipeline::run_pipeline<kQueueCapacity, BookType>(
        [&](QueueProducer<kQueueCapacity>& p) {
            itch::parse_stream(src.data(), src.size(), p);
        },
        affinity_for(pin));
    const auto t1 = std::chrono::steady_clock::now();
    report(stats.builder, src.size(), stats.frames,
           std::chrono::duration<double>(t1 - t0).count(), stats.max_occupancy, pin);
    return 0;
}

template <typename BookType>
int run_gzip(const std::string& path, bool pin) {
    io::GzipSource src(path);
    const auto t0 = std::chrono::steady_clock::now();
    RunStats<BookType> stats = pipeline::run_pipeline<kQueueCapacity, BookType>(
        [&](QueueProducer<kQueueCapacity>& p) { src.run(p); }, affinity_for(pin));
    const auto t1 = std::chrono::steady_clock::now();
    report(stats.builder, src.bytes_decompressed(), stats.frames,
           std::chrono::duration<double>(t1 - t0).count(), stats.max_occupancy, pin);
    return 0;
}

template <typename BookType>
int run_legacy(const std::string& path, bool pin) {
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
    return run<BookType>(buf.data(), buf.size(), pin);
}

// Same synthetic session as replay_main.cpp's selftest(), so the two
// binaries are directly comparable on identical input.
template <typename BookType>
int selftest(bool pin) {
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
    return run<BookType>(buf.data(), buf.size(), pin);
}

template <typename BookType>
int dispatch_args(int argc, char** argv, bool pin) {
    if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0) return selftest<BookType>(pin);
    if (argc == 3 && std::strcmp(argv[1], "--legacy") == 0) return run_legacy<BookType>(argv[2], pin);
    if (argc != 2) {
        std::fprintf(stderr,
                     "usage: %s [--map] [--pin] <file>   mmap (or gz-stream, if the name ends in\n"
                     "                            .gz) and replay a NASDAQ ITCH 5.0 file through\n"
                     "                            the parser-thread/book-builder-thread pipeline\n"
                     "       %s [--map] [--pin] --legacy <file>   read the whole file into memory\n"
                     "                            first, then replay\n"
                     "       %s [--map] [--pin] --selftest        replay a built-in synthetic session\n"
                     "       --map forces the std::map-based OrderBook instead of the default\n"
                     "               LadderBook — an explicit A/B option, not a recommended default.\n"
                     "       --pin  pins the parser thread to core 0 and the book-builder thread\n"
                     "               to core 1 (a real hard pin on Linux; a best-effort scheduling\n"
                     "               hint only on macOS — see README's threaded-pipeline section).\n",
                     argv[0], argv[0], argv[0]);
        return 2;
    }

    const std::string path = argv[1];
    const bool is_gz = path.size() >= 3 && path.compare(path.size() - 3, 3, ".gz") == 0;
    try {
        return is_gz ? run_gzip<BookType>(path, pin) : run_mmap<BookType>(path, pin);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}

bool consume_flag(int& argc, char** argv, const char* flag) {
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
    const bool use_map = consume_flag(argc, argv, "--map");
    const bool pin = consume_flag(argc, argv, "--pin");
    return use_map ? dispatch_args<book::OrderBook>(argc, argv, pin)
                   : dispatch_args<book::LadderBook>(argc, argv, pin);
}
