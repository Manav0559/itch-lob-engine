#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <thread>

#include "pipeline/spsc_queue.hpp"

using pipeline::SpscQueue;

TEST_CASE("push/pop preserves FIFO order") {
    SpscQueue<int, 8> q;
    REQUIRE(q.push(10));
    REQUIRE(q.push(20));
    REQUIRE(q.push(30));

    int v = 0;
    REQUIRE(q.pop(v));
    CHECK(v == 10);
    REQUIRE(q.pop(v));
    CHECK(v == 20);
    REQUIRE(q.pop(v));
    CHECK(v == 30);
}

TEST_CASE("push until full returns false without corrupting state") {
    // Capacity 4 holds 3 live elements: one slot is always kept empty so
    // push()'s "next == head" full check can never collide with pop()'s
    // "head == tail" empty check on an all-full buffer.
    SpscQueue<int, 4> q;
    REQUIRE(q.push(1));
    REQUIRE(q.push(2));
    REQUIRE(q.push(3));
    CHECK_FALSE(q.push(4));   // full: the 4th slot is the reserved gap
    CHECK_FALSE(q.push(5));   // still full, repeated calls stay false

    int v = 0;
    REQUIRE(q.pop(v));
    CHECK(v == 1);
    REQUIRE(q.pop(v));
    CHECK(v == 2);
    REQUIRE(q.pop(v));
    CHECK(v == 3);
    CHECK_FALSE(q.pop(v));    // drained; nothing corrupted by the earlier full pushes
}

TEST_CASE("pop until empty returns false") {
    SpscQueue<int, 4> q;
    int v = 0;
    CHECK_FALSE(q.pop(v));    // empty from the start

    REQUIRE(q.push(42));
    REQUIRE(q.pop(v));
    CHECK(v == 42);
    CHECK_FALSE(q.pop(v));    // empty again after draining
}

TEST_CASE("push/pop cycling around the wraparound boundary stays correct") {
    // Capacity 4 wraps every 3 pushes (one slot reserved). Cycle push,pop
    // repeatedly well past the point where head_/tail_ wrap past the array
    // bound multiple times, checking FIFO order survives every wrap.
    SpscQueue<int, 4> q;
    int next_push = 0;
    int next_pop = 0;

    for (int round = 0; round < 50; ++round) {
        REQUIRE(q.push(next_push));
        ++next_push;
        REQUIRE(q.push(next_push));
        ++next_push;

        int v = 0;
        REQUIRE(q.pop(v));
        CHECK(v == next_pop);
        ++next_pop;
        REQUIRE(q.pop(v));
        CHECK(v == next_pop);
        ++next_pop;
    }
    CHECK(q.size() == 0);
}

TEST_CASE("size() reflects occupancy across pushes and pops") {
    SpscQueue<int, 8> q;
    CHECK(q.size() == 0);
    REQUIRE(q.push(1));
    REQUIRE(q.push(2));
    CHECK(q.size() == 2);
    int v = 0;
    REQUIRE(q.pop(v));
    CHECK(q.size() == 1);
}

// Real cross-thread stress test: one producer thread pushes N sequential
// integers, one consumer thread pops and checks strict monotonic order. N is
// large enough (several million) to wrap the ring buffer many thousands of
// times and give a real memory-ordering bug (e.g. seq_cst-only-looking-
// correct-by-luck, or a missing acquire/release pairing) a genuine chance to
// surface as a torn/stale read rather than passing by accident on a single
// iteration. This test's PASS is not a proof of correctness under all
// interleavings — `-fsanitize=thread` is the right tool to actually verify
// the acquire/release pairing is race-free; that's a manual/CI check, not
// something wired into this binary.
TEST_CASE("concurrent producer/consumer preserves strict order under wraparound",
          "[concurrency]") {
    constexpr std::size_t kCapacity = 1024;
    constexpr int kCount = 5'000'000;

    SpscQueue<int, kCapacity> q;
    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i) {
            while (!q.push(i)) std::this_thread::yield();
        }
    });

    int next_expected = 0;
    bool ok = true;
    std::thread consumer([&] {
        int v = 0;
        while (next_expected < kCount) {
            if (q.pop(v)) {
                if (v != next_expected) {
                    ok = false;
                    break;
                }
                ++next_expected;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    CHECK(ok);
    CHECK(next_expected == kCount);
}
