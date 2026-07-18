#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <vector>

#include "itch/encode.hpp"
#include "itch/parser.hpp"

namespace {

// Records every decoded event verbatim so tests can assert byte-exact
// round-trips through encode -> frame -> parse -> decode.
struct Recorder {
    std::vector<itch::AddOrder> adds;
    std::vector<itch::OrderExecuted> execs;
    std::vector<itch::OrderExecutedPrice> exec_prices;
    std::vector<itch::OrderCancel> cancels;
    std::vector<itch::OrderDelete> deletes;
    std::vector<itch::OrderReplace> replaces;
    std::vector<std::pair<char, std::size_t>> others;

    void on_add(const itch::AddOrder& m) { adds.push_back(m); }
    void on_execute(const itch::OrderExecuted& m) { execs.push_back(m); }
    void on_execute_price(const itch::OrderExecutedPrice& m) { exec_prices.push_back(m); }
    void on_cancel(const itch::OrderCancel& m) { cancels.push_back(m); }
    void on_delete(const itch::OrderDelete& m) { deletes.push_back(m); }
    void on_replace(const itch::OrderReplace& m) { replaces.push_back(m); }
    void on_other(char t, std::size_t len) { others.emplace_back(t, len); }
};

}  // namespace

TEST_CASE("add order round-trips every field") {
    using namespace itch::encode;
    const auto buf = stream({add_order(7, 34'200'000'000'123ULL, 0xDEADBEEFCAFE01ULL,
                                       itch::Side::Buy, 250, "AAPL", 1'523'400)});
    Recorder r;
    REQUIRE(itch::parse_stream(buf.data(), buf.size(), r) == 1);
    REQUIRE(r.adds.size() == 1);
    const auto& m = r.adds[0];
    CHECK(m.hdr.locate == 7);
    CHECK(m.hdr.timestamp == 34'200'000'000'123ULL);
    CHECK(m.ref == 0xDEADBEEFCAFE01ULL);
    CHECK(m.side == itch::Side::Buy);
    CHECK(m.shares == 250);
    CHECK(std::strcmp(m.stock, "AAPL") == 0);  // wire padding trimmed
    CHECK(m.price == 1'523'400);
}

TEST_CASE("executed, executed-with-price, cancel, delete, replace round-trip") {
    using namespace itch::encode;
    const auto buf = stream({
        executed(3, 100, 42, 500, 900'001),
        executed_price(3, 110, 42, 100, 900'002, true, 999'900),
        cancel(3, 120, 43, 75),
        del(3, 130, 44),
        replace(3, 140, 45, 46, 800, 1'000'100),
    });
    Recorder r;
    REQUIRE(itch::parse_stream(buf.data(), buf.size(), r) == 5);

    REQUIRE(r.execs.size() == 1);
    CHECK(r.execs[0].ref == 42);
    CHECK(r.execs[0].shares == 500);
    CHECK(r.execs[0].match == 900'001);

    REQUIRE(r.exec_prices.size() == 1);
    CHECK(r.exec_prices[0].printable);
    CHECK(r.exec_prices[0].price == 999'900);

    REQUIRE(r.cancels.size() == 1);
    CHECK(r.cancels[0].canceled == 75);

    REQUIRE(r.deletes.size() == 1);
    CHECK(r.deletes[0].ref == 44);

    REQUIRE(r.replaces.size() == 1);
    CHECK(r.replaces[0].orig_ref == 45);
    CHECK(r.replaces[0].new_ref == 46);
    CHECK(r.replaces[0].shares == 800);
    CHECK(r.replaces[0].price == 1'000'100);
}

TEST_CASE("unknown message types are skipped via frame length, stream stays in sync") {
    using namespace itch::encode;
    Msg noii;  // 'I' — a real ITCH type this engine deliberately doesn't decode
    header(noii, 'I', 5, 200);
    for (int i = 0; i < 39; ++i) noii.push_back(0);  // arbitrary payload

    const auto buf = stream({
        add_order(1, 100, 1, itch::Side::Sell, 10, "TSLA", 2'000'000),
        noii,
        add_order(1, 300, 2, itch::Side::Sell, 20, "TSLA", 2'000'100),
    });
    Recorder r;
    REQUIRE(itch::parse_stream(buf.data(), buf.size(), r) == 3);
    REQUIRE(r.adds.size() == 2);          // both adds survived the unknown frame
    REQUIRE(r.others.size() == 1);
    CHECK(r.others[0].first == 'I');
}

TEST_CASE("known type with wrong frame length is rejected, not misparsed") {
    using namespace itch::encode;
    Msg truncated_add = add_order(1, 100, 1, itch::Side::Buy, 10, "TSLA", 2'000'000);
    truncated_add.resize(20);  // 'A' must be 36 bytes; this frame says 20

    const auto buf = stream({truncated_add,
                             del(1, 200, 99)});
    Recorder r;
    REQUIRE(itch::parse_stream(buf.data(), buf.size(), r) == 2);
    CHECK(r.adds.empty());
    REQUIRE(r.others.size() == 1);        // corrupt frame surfaced, not decoded
    CHECK(r.others[0].first == 'A');
    REQUIRE(r.deletes.size() == 1);       // and the stream stayed in sync
}

TEST_CASE("truncated tail terminates cleanly") {
    using namespace itch::encode;
    auto buf = stream({add_order(1, 100, 1, itch::Side::Buy, 10, "IBM", 1'000'000)});
    const auto full = buf;

    SECTION("partial payload") {
        buf.resize(full.size() - 5);
        Recorder r;
        CHECK(itch::parse_stream(buf.data(), buf.size(), r) == 0);
        CHECK(r.adds.empty());
    }
    SECTION("lone length byte") {
        buf = full;
        buf.push_back(0x00);  // half a length prefix dangling at the end
        Recorder r;
        CHECK(itch::parse_stream(buf.data(), buf.size(), r) == 1);
        REQUIRE(r.adds.size() == 1);
    }
}
