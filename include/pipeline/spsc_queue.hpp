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
// Cached indices: each side keeps a private copy of the other side's index
// and only re-reads the shared atomic when that copy says the queue looks
// full (producer) or empty (consumer). In steady state a push touches only
// the producer's own cache line and a pop only the consumer's, instead of
// pulling the other core's line across on every operation. A stale cache
// is always conservative: the real head only moves forward, so a stale
// cached head can only make the queue look fuller than it is (never let the
// producer overwrite an unread slot), and symmetrically for the consumer.
//
// Capacity must be a power of two so index wraparound is `& (Capacity - 1)`
// instead of `% Capacity` — a mask the compiler can fold into the load/store
// addressing instead of an integer divide on the hot path.
template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2, "one slot is always left empty to tell full from empty");
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
        if (next == cached_head_) {
            // Looks full from the cache: refresh it. Acquire pairs with the
            // consumer's release store of head_, so the slot it freed is
            // really done being read before we overwrite it.
            cached_head_ = head_.load(std::memory_order_acquire);
            if (next == cached_head_) return false;  // full
        }
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
        if (head == cached_tail_) {
            // Looks empty from the cache: refresh it. Acquire pairs with the
            // producer's release store of tail_, making every slot up to the
            // new tail visible before we read it.
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head == cached_tail_) return false;  // empty
        }
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

    static constexpr std::size_t kCacheLine = 64;

    alignas(kCacheLine) std::array<T, Capacity> buf_{};
    // Consumer line: head_ (written by the consumer, read by the producer
    // only when its cached copy says "full") plus the consumer-private
    // cached_tail_.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    std::size_t cached_tail_ = 0;
    // Producer line: tail_ plus the producer-private cached_head_. Separate
    // lines mean a push and a concurrent pop never contend on the same line
    // unless one side has to refresh its cache.
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    std::size_t cached_head_ = 0;
    // No trailing pad needed: the alignas members make alignof(SpscQueue)
    // 64, so sizeof rounds up to a whole line and nothing that follows the
    // queue can share the producer's line.
};

}  // namespace pipeline
