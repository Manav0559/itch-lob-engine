#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "book/order_book.hpp"
#include "itch/encode.hpp"
#include "itch/parser.hpp"
#include "net/moldudp64.hpp"
#include "net/multicast_receiver.hpp"
#include "net/multicast_test_sender.hpp"

// Exercises net::MoldUdp64Receiver's sequence-gap detection and gap-fill
// against net::MoldUdp64Sender using a simulated lossy sender (a
// caller-supplied drop predicate — see MoldUdp64Sender::send_all) rather
// than a real flaky network, the same loopback-simulation approach
// tests/test_multicast_receiver.cpp uses for the no-loss baseline.
namespace {

// Same fixture as test_multicast_receiver.cpp / multicast_sender_main.cpp's
// synthetic session: two symbols, adds through replaces, plus one message
// type the book doesn't decode. 9 messages total (1 'S' + 8 order events),
// so sequence numbers 1..9 are in play with 10 marking end-of-session.
std::vector<std::uint8_t> synthetic_session() {
    using namespace itch::encode;
    Msg system_event;
    header(system_event, 'S', 0, 1);
    system_event.push_back('O');

    return stream({
        system_event,
        add_order(1, 100, 1001, itch::Side::Buy, 300, "AAPL", 1'500'000),
        add_order(1, 110, 1002, itch::Side::Buy, 200, "AAPL", 1'499'900),
        add_order(1, 120, 2001, itch::Side::Sell, 500, "AAPL", 1'500'100),
        add_order(2, 130, 3001, itch::Side::Buy, 1000, "MSFT", 4'200'000),
        executed(1, 140, 1001, 300, 90001),
        cancel(1, 150, 2001, 100),
        replace(2, 160, 3001, 3002, 800, 4'199'500),
        del(1, 170, 1002),
    });
}

struct Recorder {
    std::unordered_map<std::uint16_t, book::OrderBook> books;

    void on_add(const itch::AddOrder& m) { books[m.hdr.locate].add(m.ref, m.side, m.shares, m.price); }
    void on_execute(const itch::OrderExecuted& m) { books[m.hdr.locate].execute(m.ref, m.shares); }
    void on_execute_price(const itch::OrderExecutedPrice&) {}
    void on_cancel(const itch::OrderCancel& m) { books[m.hdr.locate].cancel(m.ref, m.canceled); }
    void on_delete(const itch::OrderDelete& m) { books[m.hdr.locate].remove(m.ref); }
    void on_replace(const itch::OrderReplace& m) {
        books[m.hdr.locate].replace(m.orig_ref, m.new_ref, m.shares, m.price);
    }
    void on_other(char, std::size_t) {}
};

void assert_full_book(const Recorder& r) {
    REQUIRE(r.books.size() == 2);

    const book::OrderBook& aapl = r.books.at(1);
    CHECK(aapl.open_orders() == 1);
    CHECK(aapl.bid_levels() == 0);
    CHECK(aapl.ask_levels() == 1);
    CHECK_FALSE(aapl.best_bid().has_value());
    const auto aapl_ask = aapl.best_ask();
    REQUIRE(aapl_ask.has_value());
    CHECK(aapl_ask->price == 1'500'100);
    CHECK(aapl_ask->shares == 400);

    const book::OrderBook& msft = r.books.at(2);
    CHECK(msft.open_orders() == 1);
    CHECK(msft.ask_levels() == 0);
    CHECK(msft.bid_levels() == 1);
    CHECK_FALSE(msft.best_ask().has_value());
    const auto msft_bid = msft.best_bid();
    REQUIRE(msft_bid.has_value());
    CHECK(msft_bid->price == 4'199'500);
    CHECK(msft_bid->shares == 800);
}

// Shared skeleton for every test below: constructs sender+receiver on the
// given ports/session, runs `send_all(drop)` on a background thread
// alongside a request-server thread honoring gap-fill requests, polls the
// receiver on the calling thread until the session ends or a deadline
// passes, and returns (frames, recorder) — or SKIPs (via SUCCEED, see
// test_multicast_receiver.cpp's comment on why SUCCEED rather than SKIP)
// if this sandbox's network doesn't deliver multicast at all.
struct Result {
    bool available = false;  // false => this sandbox's network doesn't deliver multicast at all
    std::uint64_t frames = 0;
    std::uint64_t gap_fill_requests_sent = 0;
    std::uint64_t next_seq = 0;
    std::size_t pending_count = 0;
    Recorder recorder;
};

template <typename ShouldDrop>
Result run_session(std::uint16_t data_port, std::uint16_t request_port, const std::string& session_name,
                   ShouldDrop drop) {
    const std::string group = "239.255.0.1";
    const auto session = net::moldudp64::make_session(session_name);
    constexpr auto kTimeout = std::chrono::milliseconds(200);
    constexpr auto kOverallDeadline = std::chrono::seconds(8);

    Result result;

    std::unique_ptr<net::MoldUdp64Receiver> receiver;
    try {
        receiver = std::make_unique<net::MoldUdp64Receiver>(group, data_port, "127.0.0.1", request_port,
                                                             session, kTimeout, kTimeout);
    } catch (const std::exception&) {
        return result;  // available == false
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::unique_ptr<net::MoldUdp64Sender> sender;
    try {
        sender = std::make_unique<net::MoldUdp64Sender>(group, data_port, request_port, session);
    } catch (const std::exception&) {
        return result;
    }
    sender->load(synthetic_session());

    std::atomic<bool> stop_serving{false};
    std::exception_ptr sender_error;
    std::thread sender_thread([&] {
        try {
            sender->send_all(drop);
        } catch (...) {
            sender_error = std::current_exception();
        }
    });
    std::thread request_server_thread([&] {
        while (!stop_serving.load()) {
            sender->serve_one_request(std::chrono::milliseconds(100));
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + kOverallDeadline;
    while (!receiver->session_ended() && std::chrono::steady_clock::now() < deadline) {
        result.frames += receiver->poll(result.recorder);
    }

    sender_thread.join();
    stop_serving = true;
    request_server_thread.join();

    if (sender_error) return result;  // available == false

    result.available = receiver->session_ended();
    result.gap_fill_requests_sent = receiver->gap_fill_requests_sent();
    result.next_seq = receiver->next_seq();
    result.pending_count = receiver->pending_count();
    return result;
}

}  // namespace

TEST_CASE("MoldUdp64Receiver detects a single dropped packet and recovers "
          "the full book via gap-fill",
          "[concurrency]") {
    // A port pair distinct from both the demo binaries' defaults
    // (12345/12346) and test_multicast_receiver.cpp's (12399/12398).
    const auto result = run_session(12397, 12396, "GAPTEST001", [](std::uint64_t seq) {
        return seq == 3;  // drop the third message (AAPL's second bid, "A")
    });

    if (!result.available) {
        SUCCEED("multicast unavailable in this environment (see test_multicast_receiver.cpp "
                "for why this is a clean skip, not a failure)");
        return;
    }

    CHECK(result.gap_fill_requests_sent >= 1);
    CHECK(result.pending_count == 0);
    CHECK(result.next_seq == 10);  // 9 messages, 1-based sequence numbers, +1 past the last
    REQUIRE(result.frames == 9);
    assert_full_book(result.recorder);
}

TEST_CASE("MoldUdp64Receiver recovers from multiple, non-contiguous dropped "
          "packets in the same session",
          "[concurrency]") {
    const std::unordered_set<std::uint64_t> dropped = {2, 5, 8};
    const auto result = run_session(12395, 12394, "GAPTEST002",
                                    [&](std::uint64_t seq) { return dropped.count(seq) > 0; });

    if (!result.available) {
        SUCCEED("multicast unavailable in this environment");
        return;
    }

    CHECK(result.gap_fill_requests_sent >= dropped.size());
    CHECK(result.pending_count == 0);
    CHECK(result.next_seq == 10);
    REQUIRE(result.frames == 9);
    assert_full_book(result.recorder);
}

TEST_CASE("MoldUdp64Receiver ignores heartbeats without disturbing sequencing "
          "or counting them as frames",
          "[concurrency]") {
    const std::string group = "239.255.0.1";
    const std::uint16_t data_port = 12393;
    const std::uint16_t request_port = 12392;
    const auto session = net::moldudp64::make_session("GAPTEST003");
    constexpr auto kTimeout = std::chrono::milliseconds(200);
    constexpr auto kOverallDeadline = std::chrono::seconds(5);

    std::unique_ptr<net::MoldUdp64Receiver> receiver;
    try {
        receiver = std::make_unique<net::MoldUdp64Receiver>(group, data_port, "127.0.0.1", request_port,
                                                             session, kTimeout, kTimeout);
    } catch (const std::exception& e) {
        SUCCEED("multicast unavailable in this environment (join failed): " << e.what());
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::exception_ptr sender_error;
    std::thread sender_thread([&] {
        try {
            net::MoldUdp64Sender sender(group, data_port, request_port, session);
            sender.load(synthetic_session());
            sender.send_heartbeat(1);  // before anything has been sent: next seq is 1
            sender.send_all();
        } catch (...) {
            sender_error = std::current_exception();
        }
    });

    Recorder r;
    std::uint64_t frames = 0;
    const auto deadline = std::chrono::steady_clock::now() + kOverallDeadline;
    while (!receiver->session_ended() && std::chrono::steady_clock::now() < deadline) {
        frames += receiver->poll(r);
    }
    sender_thread.join();

    if (sender_error) {
        SUCCEED("multicast unavailable in this environment (send failed)");
        return;
    }
    if (!receiver->session_ended()) {
        SUCCEED("multicast unavailable in this environment (session never completed)");
        return;
    }

    CHECK(receiver->gap_fill_requests_sent() == 0);  // heartbeat must not read as a gap
    REQUIRE(frames == 9);
    assert_full_book(r);
}

// Exercises net::MoldUdp64Receiver::kMaxPendingPackets: a sender racing far
// ahead of a stalled/slow receiver (here, one that never supplies the one
// sequence number -- 1 -- the receiver actually needs) must not grow
// `pending_` without bound. Unlike the gap-fill tests above, nothing ever
// answers the receiver's retransmission requests here on purpose, so every
// one of these packets stays buffered (never drains) for the life of the
// test -- exactly the failure mode include/net/multicast_receiver.hpp's cap
// exists for.
TEST_CASE("MoldUdp64Receiver caps pending_ against a sender racing far ahead "
          "of a stalled receiver, dropping the overflow instead of growing "
          "unbounded",
          "[concurrency]") {
    const std::string group = "239.255.0.1";
    const std::uint16_t data_port = 12391;
    const std::uint16_t request_port = 12390;
    const auto session = net::moldudp64::make_session("CAPTEST01");
    // A short gap-fill timeout: every one of these packets leaves an open
    // gap at seq 1 (nothing ever replies to the resulting retransmission
    // requests), so net::MoldUdp64Receiver::resolve_gaps() runs its full
    // kMaxGapFillAttempts wait on every single one of them -- keeping this
    // small keeps the test itself fast without changing what's being
    // verified (the cap on pending_'s size, not gap-fill timing).
    constexpr auto kGapFillTimeout = std::chrono::milliseconds(2);
    constexpr auto kDataTimeout = std::chrono::milliseconds(50);

    std::unique_ptr<net::MoldUdp64Receiver> receiver;
    try {
        receiver = std::make_unique<net::MoldUdp64Receiver>(group, data_port, "127.0.0.1", request_port,
                                                             session, kDataTimeout, kGapFillTimeout);
    } catch (const std::exception& e) {
        SUCCEED("multicast unavailable in this environment (join failed): " << e.what());
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // More distinct out-of-range sequence numbers than the cap, all past
    // seq 1 (the only one that would ever let anything drain), so pending_
    // would grow past the cap here if the cap didn't exist.
    const std::uint64_t kBurst = net::MoldUdp64Receiver::kMaxPendingPackets + 64;
    std::exception_ptr sender_error;
    std::thread sender_thread([&] {
        try {
            net::MoldUdp64Sender sender(group, data_port, request_port, session);
            for (std::uint64_t seq = 2; seq < 2 + kBurst; ++seq) {
                sender.send_raw(seq, 1);
            }
        } catch (...) {
            sender_error = std::current_exception();
        }
    });

    struct NullHandler {
        void on_add(const itch::AddOrder&) {}
        void on_execute(const itch::OrderExecuted&) {}
        void on_execute_price(const itch::OrderExecutedPrice&) {}
        void on_cancel(const itch::OrderCancel&) {}
        void on_delete(const itch::OrderDelete&) {}
        void on_replace(const itch::OrderReplace&) {}
        void on_other(char, std::size_t) {}
    } handler;

    // Polls exactly kBurst times (once per packet sent above) rather than
    // stopping the instant pending_count() reaches the cap -- reaching the
    // cap only proves growth stopped, not that the *overflow* was actually
    // refused, so this keeps consuming the rest of the burst past the cap
    // to prove drops happen instead of pending_ quietly growing further.
    // Generous overall deadline relative to the worst case (kBurst accepts,
    // each potentially paying the full kMaxGapFillAttempts *
    // kGapFillTimeout before the next packet is even read off the socket).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (std::uint64_t i = 0; i < kBurst && std::chrono::steady_clock::now() < deadline; ++i) {
        receiver->poll(handler);
    }
    sender_thread.join();

    if (sender_error) {
        SUCCEED("multicast unavailable in this environment (send failed)");
        return;
    }
    if (receiver->pending_count() < net::MoldUdp64Receiver::kMaxPendingPackets) {
        SUCCEED("multicast unavailable/unreliable enough in this environment that fewer than "
                "the cap's worth of packets arrived (see test_multicast_receiver.cpp for why "
                "this is a clean skip, not a failure)");
        return;
    }

    // The cap held (never exceeded), and some of the burst was actually
    // refused rather than silently accepted past it.
    CHECK(receiver->pending_count() == net::MoldUdp64Receiver::kMaxPendingPackets);
    CHECK(receiver->dropped_pending_count() > 0);
    CHECK(receiver->gap_fill_requests_sent() > 0);  // the open gap at seq 1 was genuinely retried
}
