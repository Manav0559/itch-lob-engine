#pragma once
#include "exec/types.hpp"

namespace exec {

// CRTP base: compile-time dispatch, no vtable, no indirect call on a loop
// that runs on every quote change and every print. This mirrors the
// itch::parse_stream Handler convention already in this codebase (a
// duck-typed callback set resolved at compile time) rather than introducing
// virtual dispatch into the one place in this engine that can't afford it.
//
// Derived must implement:
//   void on_bbo_change_impl(const Bbo&);
//   void on_trade_tick_impl(const TradeTick&);
//
// Both are called synchronously off the book/tape. A strategy that wants to
// act on an event pushes into its own ChildOrderQueue member from inside
// the _impl call; the caller drains the queue after control returns. Twap
// implements both as no-ops (it is time-driven, not book/tape-driven) so it
// satisfies the same interface as the volume-reactive Vwap/Pov and all
// three can share one dispatch loop.
template <typename Derived>
class ExecutionStrategy {
public:
    void on_bbo_change(const Bbo& bbo) {
        static_cast<Derived*>(this)->on_bbo_change_impl(bbo);
    }
    void on_trade_tick(const TradeTick& tick) {
        static_cast<Derived*>(this)->on_trade_tick_impl(tick);
    }

protected:
    ExecutionStrategy() = default;
    ~ExecutionStrategy() = default;
};

}  // namespace exec
