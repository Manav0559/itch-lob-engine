#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace pipeline {

// Lock-free single-producer/single-consumer ring buffer.
//
// Ordering: head_/tail_ are std::atomic<std::size_t> but this is NOT a
// seq_cst queue. A single fixed producer and a single fixed consumer is
// exactly the textbook SPSC case: each side only ever needs to (a) read the
// *other* side's index to check available space, and (b) publish its own
// index after writing/reading a slot. Acquire/release gives that handoff for
// free — release on the writing side makes the slot's contents visible to
// whoever acquires the updated index, relaxed loads are enough for a side to
// re-check its own index, and there is no third party that could observe the
// two atomics out of order the way seq_cst's total order guards against.
// Paying for seq_cst here buys nothing beyond what acquire/release already
// gives two fixed threads, only extra fences on every push/pop.
//
// Capacity must be a power of two so index wraparound is `& (Capacity - 1)`
// instead of `% Capacity` — a mask the compiler can fold into the load/store
// addressing instead of an integer divide on the hot path.
template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscQueue carries POD messages; owning types would need a "
                  "destructor path this queue doesn't provide");

public:
    // Non-blocking: returns false (buffer full) instead of overwriting an
    // unconsumed slot.
    bool push(const T& v) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (tail + 1) & kMask;
        // Acquire: must observe the consumer's latest pop before deciding
        // the buffer is full, or we'd reject a push into a slot that's
        // actually free.
        if (next == head_.load(std::memory_order_acquire)) return false;  // full
        buf_[tail] = v;
        // Release: publishes both the new tail index and the slot write
        // above it — the consumer's acquire load of tail_ is what makes
        // buf_[tail] visible on its side.
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Non-blocking: returns false (buffer empty) rather than blocking for a
    // producer that may not come.
    bool pop(T& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        // Acquire: pairs with the producer's release store of tail_, making
        // the slot it just wrote visible before we read it.
        if (head == tail_.load(std::memory_order_acquire)) return false;  // empty
        out = buf_[head];
        // Release: publishes the freed slot to the producer's next
        // capacity check (its acquire load of head_).
        head_.store((head + 1) & kMask, std::memory_order_release);
        return true;
    }

    // Instantaneous occupancy. Not part of the push/pop hot path — a
    // backpressure-monitoring accessor a caller can poll from either thread
    // to characterize how full the buffer got (see replay_threaded_main's
    // max-occupancy tracking). Acquire on both loads: a snapshot is
    // inherently racy against a concurrently running producer/consumer, but
    // acquire ordering still guarantees it never reads a slot index without
    // also seeing the writes that index publishes.
    std::size_t size() const {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return (tail - head) & kMask;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    std::array<T, Capacity> buf_{};
    // Separate cache lines: the producer only ever writes tail_ (and reads
    // head_), the consumer only ever writes head_ (and reads tail_). Without
    // padding, both indices would share a line and every push/pop would
    // force a cache-coherency round trip between cores even though the two
    // threads never touch the same logical field.
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace pipeline
