#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "book/order_locator.hpp"

using book::OrderLocator;
using book::RestingOrder;
using itch::Side;

TEST_CASE("locator: insert, find, erase, duplicate refs") {
    OrderLocator loc(8);
    REQUIRE(loc.insert(42, RestingOrder{100, 1'000'000, Side::Buy}));
    CHECK_FALSE(loc.insert(42, RestingOrder{5, 5, Side::Sell}));  // duplicate: unchanged
    REQUIRE(loc.find(42) != nullptr);
    CHECK(loc.find(42)->shares == 100);
    CHECK(loc.find(42)->side == Side::Buy);
    CHECK(loc.size() == 1);

    loc.find(42)->shares = 40;  // execute in place
    CHECK(loc.find(42)->shares == 40);

    RestingOrder out{};
    REQUIRE(loc.extract(42, out));
    CHECK(out.shares == 40);
    CHECK(out.price == 1'000'000);
    CHECK(loc.find(42) == nullptr);
    CHECK_FALSE(loc.erase(42));
    CHECK(loc.size() == 0);
}

TEST_CASE("locator: ref 0 and UINT64_MAX are ordinary keys") {
    OrderLocator loc;
    REQUIRE(loc.insert(0, RestingOrder{1, 2, Side::Sell}));
    REQUIRE(loc.insert(UINT64_MAX, RestingOrder{3, 4, Side::Buy}));
    CHECK(loc.find(0)->shares == 1);
    CHECK(loc.find(UINT64_MAX)->shares == 3);
    CHECK(loc.erase(0));
    CHECK(loc.find(UINT64_MAX) != nullptr);
}

TEST_CASE("locator: reserve() sizes the table so the hot path never rehashes") {
    OrderLocator loc(10'000);
    const std::size_t cap = loc.capacity();
    for (std::uint64_t r = 1; r <= 10'000; ++r)
        REQUIRE(loc.insert(r, RestingOrder{100, 1, Side::Buy}));
    CHECK(loc.capacity() == cap);
    CHECK(loc.growths() == 0);

    // One past the reservation's load-factor limit may grow; it is counted.
    std::uint64_t r = 10'001;
    while (loc.capacity() == cap) REQUIRE(loc.insert(r++, RestingOrder{1, 1, Side::Sell}));
    CHECK(loc.growths() == 1);
    for (std::uint64_t k = 1; k < r; ++k) REQUIRE(loc.find(k) != nullptr);
}

TEST_CASE("locator: add/delete churn matches std::unordered_map exactly") {
    // Random insert / reduce / erase / replace against a reference map,
    // with refs drawn from a small range so probe runs collide and
    // backward-shift deletion is exercised heavily.
    OrderLocator loc(64);
    std::unordered_map<std::uint64_t, RestingOrder> ref_map;
    std::mt19937_64 rng(7);
    std::uniform_int_distribution<std::uint64_t> pick_ref(1, 600);
    std::uniform_int_distribution<int> pick_op(0, 3);
    std::uint64_t next_ref = 1'000;

    for (int step = 0; step < 200'000; ++step) {
        const std::uint64_t k = pick_ref(rng);
        switch (pick_op(rng)) {
            case 0: {  // add
                const RestingOrder o{static_cast<std::uint32_t>(k), static_cast<std::uint32_t>(k * 3),
                                     (k & 1) ? Side::Buy : Side::Sell};
                const bool a = loc.insert(k, o);
                const bool b = ref_map.try_emplace(k, o).second;
                REQUIRE(a == b);
                break;
            }
            case 1: {  // execute/cancel in place
                RestingOrder* p = loc.find(k);
                const auto it = ref_map.find(k);
                REQUIRE((p != nullptr) == (it != ref_map.end()));
                if (p) {
                    p->shares /= 2;
                    it->second.shares /= 2;
                }
                break;
            }
            case 2: {  // delete
                REQUIRE(loc.erase(k) == (ref_map.erase(k) == 1));
                break;
            }
            default: {  // replace: retire k, insert a fresh ref
                RestingOrder out{};
                const bool had = loc.extract(k, out);
                const auto it = ref_map.find(k);
                REQUIRE(had == (it != ref_map.end()));
                if (had) {
                    REQUIRE(out.shares == it->second.shares);
                    ref_map.erase(it);
                    const std::uint64_t nr = next_ref++;
                    REQUIRE(loc.insert(nr, out));
                    ref_map.emplace(nr, out);
                }
                break;
            }
        }
        REQUIRE(loc.size() == ref_map.size());
    }
    for (const auto& [k, o] : ref_map) {
        const RestingOrder* p = loc.find(k);
        REQUIRE(p != nullptr);
        CHECK(p->shares == o.shares);
        CHECK(p->price == o.price);
        CHECK(p->side == o.side);
    }
}
