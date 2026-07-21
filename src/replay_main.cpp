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
#include "pipeline/dispatch_to_book.hpp"

namespace {

using pipeline::BookBuilder;

template <typename BookType>
void report(const BookBuilder<BookType>& h, std::size_t bytes, std::size_t frames, double secs) {
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
}

template <typename BookType>
int run(const std::uint8_t* data, std::size_t len) {
    BookBuilder<BookType> h;
    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t frames = itch::parse_stream(data, len, h);
    const auto t1 = std::chrono::steady_clock::now();
    report(h, len, frames, std::chrono::duration<double>(t1 - t0).count());
    return 0;
}

// Maps the whole file read-only instead of copying it into a heap buffer —
// the path for multi-GB day files, where an ifstream-into-vector read would
// double the resident memory and pay for a copy the parser doesn't need.
template <typename BookType>
int run_mmap(const std::string& path) {
    io::MmapSource src(path);
    BookBuilder<BookType> h;
    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t frames = itch::parse_stream(src.data(), src.size(), h);
    const auto t1 = std::chrono::steady_clock::now();
    report(h, src.size(), frames, std::chrono::duration<double>(t1 - t0).count());
    return 0;
}

// Decompresses .gz day files chunk by chunk instead of inflating the whole
// day into memory first.
template <typename BookType>
int run_gzip(const std::string& path) {
    io::GzipSource src(path);
    BookBuilder<BookType> h;
    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t frames = src.run(h);
    const auto t1 = std::chrono::steady_clock::now();
    report(h, src.bytes_decompressed(), frames, std::chrono::duration<double>(t1 - t0).count());
    return 0;
}

// The original whole-file-into-memory path, kept as an explicit fallback
// (--legacy) rather than removed: useful for A/B checking the mmap/gzip
// paths against a known-simple baseline on small files.
template <typename BookType>
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
    return run<BookType>(buf.data(), buf.size());
}

// A tiny in-memory session so the binary demonstrates the full pipeline
// without a data file: two symbols, adds through replaces, plus one message
// type we don't decode (must be skipped, never desynchronize).
template <typename BookType>
int selftest() {
    using namespace itch::encode;
    Msg system_event;  // 'S' (12 bytes) — a real type the book ignores
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
    return run<BookType>(buf.data(), buf.size());
}

// Every entry point above is templated on BookType so main() can pick which
// book backs the whole run via one flag, rather than duplicating each
// function body per book type.
template <typename BookType>
int dispatch_args(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0) return selftest<BookType>();
    if (argc == 3 && std::strcmp(argv[1], "--legacy") == 0) return run_legacy<BookType>(argv[2]);
    if (argc != 2) {
        std::fprintf(stderr,
                     "usage: %s [--map] <file>   mmap (or gz-stream, if the name ends in .gz)\n"
                     "                            and replay a NASDAQ ITCH 5.0 file\n"
                     "       %s [--map] --legacy <file>   read the whole file into memory first, then replay\n"
                     "       %s [--map] --selftest        replay a built-in synthetic session\n"
                     "       --map forces the std::map-based OrderBook instead of the default\n"
                     "               LadderBook (see README's benchmark table) — an explicit A/B\n"
                     "               option, not a recommended default.\n",
                     argv[0], argv[0], argv[0]);
        return 2;
    }

    const std::string path = argv[1];
    const bool is_gz = path.size() >= 3 && path.compare(path.size() - 3, 3, ".gz") == 0;
    try {
        return is_gz ? run_gzip<BookType>(path) : run_mmap<BookType>(path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}

// Strips `flag` out of argv in place (shifting later args down) and reports
// whether it was present, so the rest of main()'s argc-based parsing below
// doesn't need to know --map can appear anywhere on the command line.
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
    return use_map ? dispatch_args<book::OrderBook>(argc, argv)
                   : dispatch_args<book::LadderBook>(argc, argv);
}
