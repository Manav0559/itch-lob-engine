#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "itch/messages.hpp"

// Execution-layer PODs and fixed-capacity containers shared by every
// strategy (Twap, and the Vwap/Pov siblings to follow). Nothing in this
// header allocates: every container here is a std::array wrapper sized by
// a compile-time constant, so a strategy's memory footprint is fixed at
// construction and reviewable with sizeof(), not "however big the day gets."
namespace exec {

// Prices are integer ticks — the same implied-4-decimal std::uint32_t the
// ITCH decoders already produce (see itch::AddOrder::price) — and volumes
// are integer lots. No floating point anywhere on the execution hot path:
// float drift in a price accumulator is exactly the kind of bug that only
// shows up after weeks of unattended running.
using Price = std::uint32_t;
using Shares = std::uint32_t;
using Timestamp = std::uint64_t;  // ns since midnight, same clock as itch::Header::timestamp

// unsigned __int128 is a GCC/Clang extension, not an ISO C++ type — this
// project's -Wpedantic (part of -Wall -Wextra -Wpedantic -Werror) rejects
// the literal token `__int128` at every point it appears in a translation
// unit. AppleClang's -Wpedantic does not flag it, which is exactly why this
// went unnoticed locally and only surfaced on the Linux (GCC) CI runner.
// Several places genuinely need >64 bits of headroom (summing price*shares
// across a whole session, or a large total_shares times a large elapsed-time
// delta, can each exceed 2^64) and both compilers this project's CI matrix
// actually targets support the extension identically — so the pragma is
// isolated to this one declaration instead of repeated at every call site,
// and everything downstream just names UInt128, never the raw extension
// keyword, so the warning has nothing left to fire on.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
using UInt128 = unsigned __int128;
#pragma GCC diagnostic pop
#else
#error "This codebase requires a compiler with a 128-bit integer extension (GCC or Clang)."
#endif

// Upper bounds for the fixed-size containers below. A schedule or queue
// that needs more than this is a sizing bug at construction time, not a
// reason to reach for std::vector — see Twap's constructor.
inline constexpr std::size_t kMaxScheduleBins = 512;
inline constexpr std::size_t kMaxChildOrders = 512;

enum class OrderType : std::uint8_t { Limit, Market };

// Best bid/offer snapshot, pushed to a strategy on every quote change it
// subscribes to. price == 0 is the "no quote on this side" sentinel (a
// valid ITCH price is never zero) rather than std::optional<Quote> per
// side — keeps Bbo a flat, trivially-copyable struct.
struct Bbo {
    Timestamp ts = 0;
    Price bid_price = 0;
    Shares bid_shares = 0;
    Price ask_price = 0;
    Shares ask_shares = 0;
};

// One executed trade off the tape (ITCH 'E'/'C'). side is the resting
// (passive) order's side, matching book::OrderBook's own bookkeeping — the
// aggressor traded the opposite side.
struct TradeTick {
    Timestamp ts = 0;
    Price price = 0;
    Shares shares = 0;
    itch::Side side = itch::Side::Buy;
};

// An order a strategy wants sent. Strategies never own or allocate this
// beyond writing it into a ChildOrderQueue slot — no std::vector<ChildOrder>,
// no factory, no ownership to transfer.
struct ChildOrder {
    Timestamp ts = 0;
    itch::Side side = itch::Side::Buy;
    OrderType type = OrderType::Limit;
    Price price = 0;  // ignored when type == OrderType::Market
    Shares shares = 0;
};

// Fixed-capacity, allocation-free sink for child orders: a std::array plus
// a size, nothing else. A push() past kMaxChildOrders is a caller/sizing
// bug — surfaced as a bool return so a strategy's hot path never throws or
// unwinds, not silently dropped.
class ChildOrderQueue {
public:
    bool push(const ChildOrder& o) {
        if (size_ >= kMaxChildOrders) return false;
        buf_[size_++] = o;
        return true;
    }
    void clear() { size_ = 0; }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    bool full() const { return size_ >= kMaxChildOrders; }

    const ChildOrder* begin() const { return buf_.data(); }
    const ChildOrder* end() const { return buf_.data() + size_; }

private:
    std::array<ChildOrder, kMaxChildOrders> buf_{};
    std::size_t size_ = 0;
};

// Trivially-copyable by construction, not by accident: these are the types
// that cross the strategy/book boundary on the hot path, and a future field
// (std::string, std::optional, ...) added carelessly would silently turn a
// memcpy-able POD into something that allocates. These asserts catch that
// at the point it's introduced, not in a profiler three months later.
static_assert(std::is_trivially_copyable_v<Bbo>);
static_assert(std::is_trivially_copyable_v<TradeTick>);
static_assert(std::is_trivially_copyable_v<ChildOrder>);

}  // namespace exec
