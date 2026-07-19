#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "book/order_book.hpp"
#include "io/gzip_source.hpp"
#include "io/mmap_source.hpp"
#include "itch/encode.hpp"
#include "itch/parser.hpp"

namespace {

// Routes decoded events into one OrderBook per stock locate and keeps
// per-type counts. Unknown order refs are counted, not fatal: on a partial
// replay (or a corrupt file) they tell you how out-of-sync the book is.
struct BookBuilder {
    std::unordered_map<std::uint16_t, book::OrderBook> books;
    std::array<std::uint64_t, 256> counts{};
    std::uint64_t unknown_refs = 0;

    void bump(char t) { ++counts[static_cast<unsigned char>(t)]; }

    void on_add(const itch::AddOrder& m) {
        bump('A');
        if (!books[m.hdr.locate].add(m.ref, m.side, m.shares, m.price)) ++unknown_refs;
    }
    void on_execute(const itch::OrderExecuted& m) {
        bump('E');
        if (!books[m.hdr.locate].execute(m.ref, m.shares)) ++unknown_refs;
    }
    void on_execute_price(const itch::OrderExecutedPrice& m) {
        bump('C');
        if (!books[m.hdr.locate].execute(m.ref, m.shares)) ++unknown_refs;
    }
    void on_cancel(const itch::OrderCancel& m) {
        bump('X');
        if (!books[m.hdr.locate].cancel(m.ref, m.canceled)) ++unknown_refs;
    }
    void on_delete(const itch::OrderDelete& m) {
        bump('D');
        if (!books[m.hdr.locate].remove(m.ref)) ++unknown_refs;
    }
    void on_replace(const itch::OrderReplace& m) {
        bump('U');
        if (!books[m.hdr.locate].replace(m.orig_ref, m.new_ref, m.shares, m.price))
            ++unknown_refs;
    }
    void on_other(char type, std::size_t) { bump(type); }
};

void report(const BookBuilder& h, std::size_t bytes, std::size_t frames, double secs) {
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
}

int run(const std::uint8_t* data, std::size_t len) {
    BookBuilder h;
    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t frames = itch::parse_stream(data, len, h);
    const auto t1 = std::chrono::steady_clock::now();
    report(h, len, frames, std::chrono::duration<double>(t1 - t0).count());
    return 0;
}

// Maps the whole file read-only instead of copying it into a heap buffer —
// the path for multi-GB day files, where an ifstream-into-vector read would
// double the resident memory and pay for a copy the parser doesn't need.
int run_mmap(const std::string& path) {
    io::MmapSource src(path);
    BookBuilder h;
    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t frames = itch::parse_stream(src.data(), src.size(), h);
    const auto t1 = std::chrono::steady_clock::now();
    report(h, src.size(), frames, std::chrono::duration<double>(t1 - t0).count());
    return 0;
}

// Decompresses .gz day files chunk by chunk instead of inflating the whole
// day into memory first.
int run_gzip(const std::string& path) {
    io::GzipSource src(path);
    BookBuilder h;
    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t frames = src.run(h);
    const auto t1 = std::chrono::steady_clock::now();
    report(h, src.bytes_decompressed(), frames, std::chrono::duration<double>(t1 - t0).count());
    return 0;
}

// The original whole-file-into-memory path, kept as an explicit fallback
// (--legacy) rather than removed: useful for A/B checking the mmap/gzip
// paths against a known-simple baseline on small files.
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

// A tiny in-memory session so the binary demonstrates the full pipeline
// without a data file: two symbols, adds through replaces, plus one message
// type we don't decode (must be skipped, never desynchronize).
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
    return run(buf.data(), buf.size());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0) return selftest();
    if (argc == 3 && std::strcmp(argv[1], "--legacy") == 0) return run_legacy(argv[2]);
    if (argc != 2) {
        std::fprintf(stderr,
                     "usage: %s <file>           mmap (or gz-stream, if the name ends in .gz)\n"
                     "                            and replay a NASDAQ ITCH 5.0 file\n"
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
