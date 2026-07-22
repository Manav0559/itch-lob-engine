#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "book/ladder_book.hpp"
#include "book/order_book.hpp"
#include "io/gzip_source.hpp"
#include "io/mmap_source.hpp"
#include "itch/encode.hpp"
#include "itch/parser.hpp"
#include "net/query_server.hpp"
#include "pipeline/book_snapshot.hpp"
#include "pipeline/dispatch_to_book.hpp"

// Same single-threaded replay as replay_main.cpp, plus a live, read-only
// query surface (net::QueryServer) answering best bid/ask, depth, and open-
// order-count questions per stock-locate while the replay runs — and for as
// long afterward as --serve-seconds asks for (default: until Ctrl-C, same
// as live_replay_main's convention for a process with no other natural
// end).
//
// The ingest side (this process's main thread, running itch::parse_stream)
// never talks to the query server directly and is never blocked by it: the
// only thing crossing between them is a plain-value pipeline::LocateSnapshot
// vector, published every --publish-every messages (see
// PublishingHandler::maybe_publish below) into a pipeline::SnapshotStore
// under a mutex held only for that handoff — never for a book mutation or
// for however long the snapshot walk itself took. See
// include/pipeline/book_snapshot.hpp's header comment for the full
// rationale.
//
// Try it:
//   ./build/replay_query --selftest --publish-every 1 --pace-us 200000 &
//   printf '{"cmd":"list"}\n' | nc 127.0.0.1 12401
//   printf '{"cmd":"quote","locate":1}\n' | nc 127.0.0.1 12401
namespace {

using pipeline::BookBuilder;

// Wraps a BookBuilder<BookType> to match the exact same itch::dispatch
// Handler interface it already implements (on_add/on_execute/.../on_other),
// forwarding every call unchanged and, every `publish_every` book-touching
// messages, taking a snapshot of the (single-owner-safe, because this
// wrapper is the only thing touching it) BookTable and publishing it. A
// final unconditional publish after the whole parse completes (see run()
// below) means the served snapshot always ends up reflecting the true final
// state, regardless of where the message count landed relative to
// publish_every.
template <typename BookType>
struct PublishingHandler {
    BookBuilder<BookType>& inner;
    pipeline::SnapshotStore& store;
    std::size_t publish_every;
    std::chrono::microseconds pace;
    std::size_t since_publish = 0;

    void maybe_publish() {
        if (publish_every == 0 || ++since_publish < publish_every) return;
        since_publish = 0;
        store.publish(pipeline::snapshot_book_table(inner.books), inner.unknown_refs);
        if (pace.count() > 0) std::this_thread::sleep_for(pace);
    }

    void on_add(const itch::AddOrder& m) {
        inner.on_add(m);
        maybe_publish();
    }
    void on_execute(const itch::OrderExecuted& m) {
        inner.on_execute(m);
        maybe_publish();
    }
    void on_execute_price(const itch::OrderExecutedPrice& m) {
        inner.on_execute_price(m);
        maybe_publish();
    }
    void on_cancel(const itch::OrderCancel& m) {
        inner.on_cancel(m);
        maybe_publish();
    }
    void on_delete(const itch::OrderDelete& m) {
        inner.on_delete(m);
        maybe_publish();
    }
    void on_replace(const itch::OrderReplace& m) {
        inner.on_replace(m);
        maybe_publish();
    }
    void on_other(char type, std::size_t len) { inner.on_other(type, len); }
};

struct Options {
    std::uint16_t port = 12401;   // distinct from live_replay's 12345/12346 and the test ports
    std::size_t publish_every = 2000;
    std::chrono::microseconds pace{0};
    std::uint64_t serve_seconds = 0;  // 0 = serve until Ctrl-C
    bool use_map = false;
};

void report(std::size_t bytes, std::size_t frames, double secs, std::size_t books,
            std::size_t open_orders, std::uint64_t snapshot_version, std::uint64_t unknown_refs) {
    std::printf("bytes            %zu\n", bytes);
    std::printf("frames           %zu\n", frames);
    std::printf("elapsed          %.3f s\n", secs);
    std::printf("books            %zu\n", books);
    std::printf("open orders      %zu\n", open_orders);
    std::printf("unknown refs     %llu\n", static_cast<unsigned long long>(unknown_refs));
    std::printf("snapshot version %llu (query server always answers from this store, never\n"
                "                 the live book — see include/pipeline/book_snapshot.hpp;\n"
                "                 unknown_refs above is also part of every list/quote\n"
                "                 response — see include/net/query_server.hpp)\n",
                static_cast<unsigned long long>(snapshot_version));
}

// Set by the SIGINT handler; polled once per second while serving after
// replay completes with --serve-seconds 0 (serve forever). Same rationale as
// live_replay_main.cpp's g_stop: a live process with no natural end needs
// Ctrl-C as its stop signal.
volatile std::sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

// Blocks until --serve-seconds elapses (if nonzero) or SIGINT, whichever the
// caller asked for — the query server itself is already running in its own
// thread throughout, so this is purely "how long does main() keep it alive."
void serve(const Options& opt) {
    std::printf("query server listening on 127.0.0.1:%u — try:\n"
                "  printf '{\"cmd\":\"list\"}\\n' | nc 127.0.0.1 %u\n",
                opt.port, opt.port);
    if (opt.serve_seconds == 0) {
        std::printf("serving until Ctrl-C\n");
        std::fflush(stdout);
        while (g_stop == 0) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    } else {
        std::printf("serving for %llu more second(s)\n",
                    static_cast<unsigned long long>(opt.serve_seconds));
        std::fflush(stdout);
        for (std::uint64_t i = 0; i < opt.serve_seconds && g_stop == 0; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

template <typename BookType>
int run(const Options& opt, const std::uint8_t* data, std::size_t len) {
    pipeline::SnapshotStore store;
    net::QueryServer server(opt.port, store);
    server.start();

    BookBuilder<BookType> h;
    PublishingHandler<BookType> handler{h, store, opt.publish_every, opt.pace};

    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t frames = itch::parse_stream(data, len, handler);
    const auto t1 = std::chrono::steady_clock::now();

    // Unconditional final publish: the last (< publish_every) messages
    // parsed above may not have crossed maybe_publish()'s threshold, so
    // without this the served snapshot could be stale relative to the
    // book's true final state.
    store.publish(pipeline::snapshot_book_table(h.books), h.unknown_refs);

    std::size_t open_orders = 0;
    h.books.for_each([&](std::uint16_t, const BookType& b) { open_orders += b.open_orders(); });
    report(len, frames, std::chrono::duration<double>(t1 - t0).count(), h.books.book_count(),
          open_orders, store.version(), h.unknown_refs);

    serve(opt);
    server.stop();
    return 0;
}

template <typename BookType>
int run_mmap(const Options& opt, const std::string& path) {
    io::MmapSource src(path);
    return run<BookType>(opt, src.data(), src.size());
}

template <typename BookType>
int run_gzip(const Options& opt, const std::string& path) {
    io::GzipSource src(path);
    pipeline::SnapshotStore store;
    net::QueryServer server(opt.port, store);
    server.start();

    BookBuilder<BookType> h;
    PublishingHandler<BookType> handler{h, store, opt.publish_every, opt.pace};

    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t frames = src.run(handler);
    const auto t1 = std::chrono::steady_clock::now();

    store.publish(pipeline::snapshot_book_table(h.books), h.unknown_refs);

    std::size_t open_orders = 0;
    h.books.for_each([&](std::uint16_t, const BookType& b) { open_orders += b.open_orders(); });
    report(src.bytes_decompressed(), frames, std::chrono::duration<double>(t1 - t0).count(),
          h.books.book_count(), open_orders, store.version(), h.unknown_refs);

    serve(opt);
    server.stop();
    return 0;
}

template <typename BookType>
int run_legacy(const Options& opt, const std::string& path) {
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
    return run<BookType>(opt, buf.data(), buf.size());
}

// Same synthetic session replay_main.cpp's selftest() and
// replay_threaded_main.cpp's selftest() use, so all three demo binaries are
// directly comparable on identical input.
template <typename BookType>
int selftest(const Options& opt) {
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
    return run<BookType>(opt, buf.data(), buf.size());
}

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [options] <file>            mmap (or gz-stream) and replay a NASDAQ\n"
                 "                                       ITCH 5.0 file, serving live queries\n"
                 "       %s [options] --legacy <file>    read the whole file into memory first\n"
                 "       %s [options] --selftest         replay a built-in synthetic session\n"
                 "options:\n"
                 "  --port N            TCP port for the query server (default 12401; 0 = let\n"
                 "                      the OS pick one, printed once bound)\n"
                 "  --publish-every N   snapshot+publish to the query server every N book-\n"
                 "                      touching messages (default 2000; 0 disables periodic\n"
                 "                      publishing — only the final, post-replay snapshot is\n"
                 "                      ever served)\n"
                 "  --pace-us N         sleep N microseconds after each periodic publish\n"
                 "                      (default 0) — slows replay down enough to demo live\n"
                 "                      querying against a small file/--selftest\n"
                 "  --serve-seconds N   keep the query server up N seconds after replay\n"
                 "                      finishes, then exit (default 0 = serve until Ctrl-C)\n"
                 "  --map               force the std::map-based OrderBook instead of the\n"
                 "                      default LadderBook (see README) — an explicit A/B\n"
                 "                      option, not a recommended default\n"
                 "query protocol: one JSON object per line, newline-terminated, over TCP —\n"
                 "  {\"cmd\":\"list\"}                  every locate currently known\n"
                 "  {\"cmd\":\"quote\",\"locate\":N}       best bid/ask, depth, open orders for N\n"
                 "see include/net/query_server.hpp for the full wire format and its documented\n"
                 "scope (no auth, loopback-only, not a general JSON parser).\n",
                 argv0, argv0, argv0);
}

// Strips `flag` (with a following value argument, unless it's --map/
// --selftest/--legacy) out of argv in place, the same consume_flag idiom
// replay_main.cpp / replay_threaded_main.cpp use for their boolean flags —
// extended here to also pull out a value for the options that take one.
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
    if (consume_value_flag(argc, argv, "--port", val)) opt.port = static_cast<std::uint16_t>(std::atoi(val.c_str()));
    if (consume_value_flag(argc, argv, "--publish-every", val))
        opt.publish_every = static_cast<std::size_t>(std::strtoull(val.c_str(), nullptr, 10));
    if (consume_value_flag(argc, argv, "--pace-us", val))
        opt.pace = std::chrono::microseconds(std::strtoll(val.c_str(), nullptr, 10));
    if (consume_value_flag(argc, argv, "--serve-seconds", val))
        opt.serve_seconds = static_cast<std::uint64_t>(std::strtoull(val.c_str(), nullptr, 10));

    std::signal(SIGINT, on_sigint);

    try {
        if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0)
            return opt.use_map ? selftest<book::OrderBook>(opt) : selftest<book::LadderBook>(opt);
        if (argc == 3 && std::strcmp(argv[1], "--legacy") == 0)
            return opt.use_map ? run_legacy<book::OrderBook>(opt, argv[2])
                                : run_legacy<book::LadderBook>(opt, argv[2]);
        if (argc != 2) {
            usage(argv[0]);
            return 2;
        }

        const std::string path = argv[1];
        const bool is_gz = path.size() >= 3 && path.compare(path.size() - 3, 3, ".gz") == 0;
        if (is_gz) {
            return opt.use_map ? run_gzip<book::OrderBook>(opt, path) : run_gzip<book::LadderBook>(opt, path);
        }
        return opt.use_map ? run_mmap<book::OrderBook>(opt, path) : run_mmap<book::LadderBook>(opt, path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
