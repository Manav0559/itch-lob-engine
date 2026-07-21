#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "book/ladder_book.hpp"
#include "itch/encode.hpp"
#include "itch/parser.hpp"
#include "net/query_server.hpp"
#include "pipeline/book_snapshot.hpp"
#include "pipeline/dispatch_to_book.hpp"

// End-to-end coverage of net::QueryServer's wire protocol: a real TCP client
// connects over loopback, sends the JSON-lines requests the protocol
// supports, and the responses are checked against pipeline::LocateSnapshot
// data published ahead of time — the same SnapshotStore handoff
// tests/test_book_snapshot.cpp already covers in isolation. `--port 0` (via
// the constructor) asks the OS for an ephemeral free port, so this test
// never collides with another binary/test's fixed port (the convention
// tests/test_multicast_receiver.cpp documents for the same reason).
namespace {

using pipeline::BookBuilder;

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
        executed(1, 140, 1001, 300, 90001),           // fills the whole best bid
        cancel(1, 150, 2001, 100),                    // partial cancel, ask stays
        replace(2, 160, 3001, 3002, 800, 4'199'500),  // MSFT bid moves down
        del(1, 170, 1002),                            // AAPL book now ask-only
    });
}

// Minimal raw TCP client, local to this test: connect, send one line, read
// one line back (with a bounded timeout so a protocol bug fails the test
// instead of hanging the suite).
class TestClient {
public:
    explicit TestClient(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd_ >= 0);
        timeval tv{};
        tv.tv_sec = 2;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        // The server's accept thread starts asynchronously (QueryServer::start()
        // returns immediately) — a bounded retry loop covers the brief window
        // before its listen() backlog is actually ready to accept, without an
        // arbitrary fixed sleep.
        int rc = -1;
        for (int attempt = 0; attempt < 50 && rc != 0; ++attempt) {
            rc = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            if (rc != 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        REQUIRE(rc == 0);
    }
    ~TestClient() {
        if (fd_ >= 0) ::close(fd_);
    }
    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;

    std::string request(const std::string& line) {
        const std::string with_nl = line + "\n";
        REQUIRE(::send(fd_, with_nl.data(), with_nl.size(), 0) == static_cast<ssize_t>(with_nl.size()));

        std::string buf;
        char c;
        while (buf.empty() || buf.back() != '\n') {
            const ssize_t n = ::recv(fd_, &c, 1, 0);
            REQUIRE(n == 1);  // a timeout or closed connection would return <= 0 — fail loudly, don't hang
            buf.push_back(c);
        }
        buf.pop_back();  // trailing '\n'
        return buf;
    }

private:
    int fd_ = -1;
};

}  // namespace

TEST_CASE("QueryServer answers list/quote requests from a published snapshot over real TCP",
          "[concurrency]") {
    BookBuilder<book::LadderBook> h;
    const auto buf = synthetic_session();
    itch::parse_stream(buf.data(), buf.size(), h);

    pipeline::SnapshotStore store;
    store.publish(pipeline::snapshot_book_table(h.books));
    REQUIRE(store.version() == 1);

    net::QueryServer server(/*port=*/0, store);
    server.start();
    TestClient client(server.bound_port());

    SECTION("list returns every known locate with the current snapshot version") {
        const std::string resp = client.request(R"({"cmd":"list"})");
        CHECK(resp.find("\"snapshot_version\":1") != std::string::npos);
        CHECK(resp.find("\"locate\":1") != std::string::npos);
        CHECK(resp.find("\"locate\":2") != std::string::npos);
        CHECK(resp.find("\"error\"") == std::string::npos);
    }

    SECTION("quote for AAPL (locate 1) reports its ask-only final state") {
        const std::string resp = client.request(R"({"cmd":"quote","locate":1})");
        CHECK(resp.find("\"locate\":1") != std::string::npos);
        CHECK(resp.find("\"best_bid\":null") != std::string::npos);
        CHECK(resp.find("\"best_ask\":1500100") != std::string::npos);
        CHECK(resp.find("\"best_ask_shares\":400") != std::string::npos);
        CHECK(resp.find("\"open_orders\":1") != std::string::npos);
        CHECK(resp.find("\"bid_levels\":0") != std::string::npos);
        CHECK(resp.find("\"ask_levels\":1") != std::string::npos);
        CHECK(resp.find("\"snapshot_version\":1") != std::string::npos);
    }

    SECTION("quote for MSFT (locate 2) reports its bid-only final state") {
        const std::string resp = client.request(R"({"cmd":"quote","locate":2})");
        CHECK(resp.find("\"best_bid\":4199500") != std::string::npos);
        CHECK(resp.find("\"best_bid_shares\":800") != std::string::npos);
        CHECK(resp.find("\"best_ask\":null") != std::string::npos);
    }

    SECTION("quote for a locate never seen returns an error, not a crash") {
        const std::string resp = client.request(R"({"cmd":"quote","locate":99})");
        CHECK(resp.find("\"error\":\"unknown locate\"") != std::string::npos);
        CHECK(resp.find("\"locate\":99") != std::string::npos);
    }

    SECTION("quote without a locate field returns a specific error") {
        const std::string resp = client.request(R"({"cmd":"quote"})");
        CHECK(resp.find("\"error\":\"missing locate\"") != std::string::npos);
    }

    SECTION("an unrecognized command returns an error") {
        const std::string resp = client.request(R"({"cmd":"delete_everything"})");
        CHECK(resp.find("\"error\":\"unknown command\"") != std::string::npos);
    }

    SECTION("a line with no cmd field at all is a bad request, not a hang or crash") {
        const std::string resp = client.request(R"({"oops":true})");
        CHECK(resp.find("\"error\":\"bad request\"") != std::string::npos);
    }

    SECTION("one connection can issue multiple sequential requests") {
        const std::string first = client.request(R"({"cmd":"list"})");
        const std::string second = client.request(R"({"cmd":"quote","locate":1})");
        CHECK(first.find("\"books\"") != std::string::npos);
        CHECK(second.find("\"locate\":1") != std::string::npos);
    }

    server.stop();
}

TEST_CASE("QueryServer reflects a later publish() to already-connected and new clients alike",
          "[concurrency]") {
    pipeline::SnapshotStore store;
    net::QueryServer server(/*port=*/0, store);
    server.start();
    TestClient client(server.bound_port());

    CHECK(client.request(R"({"cmd":"list"})").find("\"snapshot_version\":0") != std::string::npos);

    pipeline::LocateSnapshot s;
    s.locate = 7;
    s.has_bid = true;
    s.bid_price = 42;
    s.bid_shares = 5;
    store.publish({s});

    const std::string resp = client.request(R"({"cmd":"quote","locate":7})");
    CHECK(resp.find("\"snapshot_version\":1") != std::string::npos);
    CHECK(resp.find("\"best_bid\":42") != std::string::npos);

    server.stop();
}

TEST_CASE("query_wire field extraction tolerates whitespace and either key order", "[snapshot]") {
    CHECK(net::query_wire::extract_string_field(R"({"cmd":"list"})", "cmd") == "list");
    CHECK(net::query_wire::extract_string_field(R"({ "cmd" : "quote" })", "cmd") == "quote");
    CHECK(net::query_wire::extract_string_field(R"({"locate":1,"cmd":"quote"})", "cmd") == "quote");
    CHECK_FALSE(net::query_wire::extract_string_field(R"({"nope":1})", "cmd").has_value());

    CHECK(net::query_wire::extract_uint_field(R"({"cmd":"quote","locate":42})", "locate") == 42u);
    CHECK(net::query_wire::extract_uint_field(R"({"locate": 7, "cmd":"quote"})", "locate") == 7u);
    CHECK_FALSE(net::query_wire::extract_uint_field(R"({"cmd":"quote"})", "locate").has_value());
    CHECK_FALSE(net::query_wire::extract_uint_field(R"({"locate":"abc"})", "locate").has_value());
}
