#pragma once
#include <type_traits>

#include "itch/messages.hpp"

namespace pipeline {

// Tagged union carrying one decoded ITCH message across the SPSC queue. A
// manual union (not std::variant) matches this codebase's existing
// preference for avoiding std library machinery with hidden cost on a hot
// path — variant's index bookkeeping and visitation overhead buy nothing
// here that a switch on `type` doesn't already give for free, and this type
// must stay trivially copyable to live in SpscQueue.
//
// `type` reuses the ITCH wire type char ('A'/'E'/'C'/'X'/'D'/'U') so it
// doubles as both the union discriminant and, unmodified, the same tag
// BookBuilder::bump() already counts by.
struct Envelope {
    char type = '\0';
    union {
        itch::AddOrder add;
        itch::OrderExecuted exec;
        itch::OrderExecutedPrice exec_price;
        itch::OrderCancel cancel;
        itch::OrderDelete del;
        itch::OrderReplace replace;
    };
};

static_assert(std::is_trivially_copyable_v<Envelope>,
              "Envelope must stay POD to travel through SpscQueue");

}  // namespace pipeline
