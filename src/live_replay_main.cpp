#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

#include "book/order_book.hpp"
#include "itch/parser.hpp"
#include "net/multicast_receiver.hpp"

// SCOPE: joins a UDP multicast group and decodes the same 2-byte-length-
// prefix framing replay_main.cpp reads from files, one frame per datagram —
// see include/net/multicast_receiver.hpp and
// include/net/multicast_test_sender.hpp for why this is a proof-of-concept
// of the delivery mechanism, not a MoldUDP64 implementation, and why it
// assumes no packet loss (fine on loopback against multicast_sender_main,
// not representative of a real exchange feed).
namespace {

// Same shape as replay_main.cpp's BookBuilder, kept as a separate local copy
// rather than shared: it's small, and this binary's decode loop (bounded
// per-datagram, periodic reporting, run-until-Ctrl-C) is different enough
// from replay_main's single-pass-over-a-buffer loop that sharing the struct
// wouldn't remove much duplication.
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

void report(const BookBuilder& h, std::uint64_t frames) {
    std::size_t open_orders = 0, bid_levels = 0, ask_levels = 0;
    for (const auto& [locate, b] : h.books) {
        open_orders += b.open_orders();
        bid_levels += b.bid_levels();
        ask_levels += b.ask_levels();
    }

    std::printf("--- frames %llu ---\n", static_cast<unsigned long long>(frames));
    std::printf("books            %zu\n", h.books.size());
    std::printf("open orders      %zu (bid levels %zu, ask levels %zu)\n", open_orders,
                bid_levels, ask_levels);
    std::printf("unknown refs     %llu\n",
                static_cast<unsigned long long>(h.unknown_refs));
    std::printf("frames by type   ");
    for (int t = 0; t < 256; ++t)
        if (h.counts[static_cast<std::size_t>(t)] > 0)
            std::printf("%c=%llu ", static_cast<char>(t),
                        static_cast<unsigned long long>(h.counts[static_cast<std::size_t>(t)]));
    std::printf("\n");
}

// Set by the SIGINT handler; polled once per recv() timeout. A live
// multicast feed has no end-of-file the way a replayed file does, so Ctrl-C
// is the only way this binary ever stops. The poll loop relies on a recv()
// timeout rather than EINTR from the signal itself: whether a blocking
// syscall interrupted by a signal returns EINTR or is transparently
// restarted is platform-dependent (macOS's libc signal() restarts by
// default), so EINTR alone isn't a portable way to break out of a blocking
// recv() across the Linux/macOS CI matrix this repo targets.
volatile std::sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

int run(const std::string& group, std::uint16_t port, std::uint64_t report_every) {
    constexpr auto kPollInterval = std::chrono::milliseconds(200);
    net::MulticastReceiver recv(group, port, kPollInterval);
    std::printf("joined %s:%u — press Ctrl-C to stop (a live feed has no natural end)\n",
                group.c_str(), port);
    std::fflush(stdout);

    BookBuilder h;
    std::vector<std::uint8_t> buf(65536);
    std::uint64_t frames = 0;

    while (g_stop == 0) {
        const ssize_t n = recv.receive(buf.data(), buf.size());
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;  // recv timeout (expected — lets us re-check g_stop) or interrupted
            std::fprintf(stderr, "recv error: %s\n", std::strerror(errno));
            break;
        }
        if (n == 0) continue;

        frames += itch::parse_stream(buf.data(), static_cast<std::size_t>(n), h);
        if (report_every > 0 && frames > 0 && frames % report_every == 0) {
            report(h, frames);
            std::fflush(stdout);
        }
    }

    std::printf("\nstopped\n");
    report(h, frames);
    std::fflush(stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string group = "239.255.0.1";
    std::uint16_t port = 12345;
    std::uint64_t report_every = 5;  // print a summary every N decoded frames
    if (argc == 4) {
        group = argv[1];
        port = static_cast<std::uint16_t>(std::atoi(argv[2]));
        report_every = static_cast<std::uint64_t>(std::atoll(argv[3]));
    } else if (argc != 1) {
        std::fprintf(stderr, "usage: %s [group port report_every]\n", argv[0]);
        std::fprintf(stderr,
                     "  joins a multicast group (default 239.255.0.1:12345) and replays\n"
                     "  decoded ITCH frames into per-symbol books until Ctrl-C.\n"
                     "  pair with multicast_sender_main to exercise this without a real feed.\n");
        return 2;
    }

    std::signal(SIGINT, on_sigint);

    try {
        return run(group, port, report_every);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
