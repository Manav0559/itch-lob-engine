#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <vector>

#include "book/ladder_book.hpp"
#include "itch/encode.hpp"
#include "itch/parser.hpp"
#include "pipeline/book_snapshot.hpp"
#include "pipeline/dispatch_to_book.hpp"

// Unit coverage for the ingest-side/query-side handoff itself
// (pipeline::snapshot_book_table + pipeline::SnapshotStore), independent of
// net::QueryServer's wire protocol (covered separately in
// tests/test_query_server.cpp).
namespace {

using pipeline::BookBuilder;

// Same synthetic session replay_main.cpp's selftest() uses: two symbols,
// adds through replaces, plus one message type the book doesn't decode.
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

std::optional<pipeline::LocateSnapshot> find(const std::vector<pipeline::LocateSnapshot>& v,
                                             std::uint16_t locate) {
    for (const auto& s : v)
        if (s.locate == locate) return s;
    return std::nullopt;
}

}  // namespace

TEST_CASE("snapshot_book_table copies exactly the fields a query needs, matching the live book",
          "[snapshot]") {
    BookBuilder<book::LadderBook> h;
    const auto buf = synthetic_session();
    itch::parse_stream(buf.data(), buf.size(), h);

    const auto snap = pipeline::snapshot_book_table(h.books);
    REQUIRE(snap.size() == 2);  // two locates seen: AAPL (1), MSFT (2)

    const auto aapl = find(snap, 1);
    REQUIRE(aapl.has_value());
    // AAPL ends ask-only: 1001 bid fully executed, 1002 bid deleted, 2001
    // ask partially canceled from 500 to 400 shares.
    CHECK_FALSE(aapl->has_bid);
    CHECK(aapl->has_ask);
    CHECK(aapl->ask_price == 1'500'100);
    CHECK(aapl->ask_shares == 400);
    CHECK(aapl->open_orders == 1);
    CHECK(aapl->bid_levels == 0);
    CHECK(aapl->ask_levels == 1);

    const auto msft = find(snap, 2);
    REQUIRE(msft.has_value());
    // MSFT ends bid-only: 3001 replaced by 3002 at a lower price/size.
    CHECK(msft->has_bid);
    CHECK(msft->bid_price == 4'199'500);
    CHECK(msft->bid_shares == 800);
    CHECK_FALSE(msft->has_ask);
    CHECK(msft->open_orders == 1);
    CHECK(msft->bid_levels == 1);
    CHECK(msft->ask_levels == 0);
}

TEST_CASE("snapshot_book_table on an empty BookTable produces an empty snapshot", "[snapshot]") {
    pipeline::BookTable<book::LadderBook> books;
    const auto snap = pipeline::snapshot_book_table(books);
    CHECK(snap.empty());
}

TEST_CASE("SnapshotStore starts at version 0 with no data, and publish() replaces the prior "
          "snapshot wholesale",
          "[snapshot]") {
    pipeline::SnapshotStore store;
    CHECK(store.version() == 0);
    CHECK(store.read_all().empty());
    CHECK_FALSE(store.find(1).has_value());

    pipeline::LocateSnapshot a;
    a.locate = 1;
    a.has_bid = true;
    a.bid_price = 100;
    a.bid_shares = 10;
    store.publish({a});

    CHECK(store.version() == 1);
    REQUIRE(store.read_all().size() == 1);
    const auto found = store.find(1);
    REQUIRE(found.has_value());
    CHECK(found->bid_price == 100);
    CHECK_FALSE(store.find(2).has_value());

    // A second publish() is a full replacement, not a merge — a locate
    // missing from the new snapshot must disappear, not linger from the
    // previous one.
    pipeline::LocateSnapshot b;
    b.locate = 2;
    b.has_ask = true;
    b.ask_price = 200;
    b.ask_shares = 20;
    store.publish({b});

    CHECK(store.version() == 2);
    REQUIRE(store.read_all().size() == 1);
    CHECK_FALSE(store.find(1).has_value());  // gone: not carried over from the first publish
    const auto found_b = store.find(2);
    REQUIRE(found_b.has_value());
    CHECK(found_b->ask_price == 200);
}
