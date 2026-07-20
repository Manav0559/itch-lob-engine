#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "book/order_book.hpp"
#include "itch/encode.hpp"
#include "itch/parser.hpp"
#include "net/multicast_receiver.hpp"
#include "net/multicast_test_sender.hpp"

namespace {

// The same synthetic session --selftest / multicast_sender_main send: two
// symbols, adds through replaces, plus one message type the book doesn't
// decode. Kept as its own local copy (small, test-only fixture) rather than
// shared across the two binaries and this test — same rationale test_io.cpp
// gives for duplicating test_parser.cpp's Recorder.
std::vector<std::uint8_t> synthetic_session() {
    using namespace itch::encode;
    Msg system_event;  // 'S' (12 bytes) — a real type the book ignores
    header(system_event, 'S', 0, 1);
    system_event.push_back('O');

    return stream({
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
}

// Routes decoded events into one OrderBook per locate, mirroring
// replay_main.cpp's BookBuilder minus the diagnostics this test doesn't need.
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

}  // namespace

TEST_CASE("multicast_receiver receives a synthetic session byte-for-byte and "
          "rebuilds the known-good book") {
    const std::string group = "239.255.0.1";
    // A port distinct from the demo binaries' default (12345), so this test
    // never collides with someone manually running multicast_sender_main /
    // live_replay_main against each other while the suite is running.
    const std::uint16_t port = 12399;

    const std::vector<std::uint8_t> framed = synthetic_session();

    // Join before sending: the multicast group membership must be in place
    // (IGMP join propagated) before any datagram is sent, or the first
    // frame(s) are silently dropped rather than received.
    //
    // Some sandboxed CI network stacks (observed on a GitHub-hosted
    // macos-latest runner) don't permit IP multicast at all — join or send
    // can fail there for reasons entirely outside this code's control. That
    // is an environment limitation to skip past, not a test failure, and
    // it must never surface as an uncaught exception: one thrown across a
    // std::thread boundary (the sender thread below) calls std::terminate,
    // which aborts the whole test binary instead of failing just this one
    // case. Both the join and the send are guarded accordingly.
    std::unique_ptr<net::MulticastReceiver> receiver;
    try {
        receiver = std::make_unique<net::MulticastReceiver>(group, port);
    } catch (const std::exception& e) {
        SKIP("multicast unavailable in this environment (join failed): " << e.what());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::exception_ptr sender_error;
    std::thread sender([&] {
        try {
            net::send_multicast_stream(group, port, framed);
        } catch (...) {
            sender_error = std::current_exception();
        }
    });

    std::vector<std::uint8_t> received;
    std::vector<std::uint8_t> buf(65536);
    while (received.size() < framed.size()) {
        const ssize_t n = receiver->receive(buf.data(), buf.size());
        if (n <= 0) break;  // let the join()+sender_error check below explain why
        received.insert(received.end(), buf.begin(), buf.begin() + n);
    }
    sender.join();

    if (sender_error) {
        try {
            std::rethrow_exception(sender_error);
        } catch (const std::exception& e) {
            SKIP("multicast unavailable in this environment (send failed): " << e.what());
        }
    }
    REQUIRE(received == framed);

    Recorder r;
    REQUIRE(itch::parse_stream(received.data(), received.size(), r) == 9);  // 1 'S' + 8 order events
    REQUIRE(r.books.size() == 2);

    // AAPL (locate 1): the 1001 bid is fully executed and erased, the 1002
    // bid is deleted, so the book ends up ask-only — the 2001 ask, partially
    // canceled from 500 to 400 shares, is the only thing left.
    const book::OrderBook& aapl = r.books.at(1);
    CHECK(aapl.open_orders() == 1);
    CHECK(aapl.bid_levels() == 0);
    CHECK(aapl.ask_levels() == 1);
    CHECK_FALSE(aapl.best_bid().has_value());
    const auto aapl_ask = aapl.best_ask();
    REQUIRE(aapl_ask.has_value());
    CHECK(aapl_ask->price == 1'500'100);
    CHECK(aapl_ask->shares == 400);

    // MSFT (locate 2): the 3001 bid is replaced by 3002 at a lower price/size
    // — same book-side, moved down — so the book stays bid-only.
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
