#pragma once
#include <cstdint>
#include <cstring>

namespace itch {

// ITCH 5.0 wire format: every integer is big-endian, prices are uint32 with
// an implied 4 decimal places, timestamps are 48-bit nanoseconds since
// midnight. Decoders below read from unaligned buffers byte-by-byte, so they
// are endian- and alignment-safe on any host.
inline std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((std::uint16_t{p[0]} << 8) | p[1]);
}
inline std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
           (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}
inline std::uint64_t be48(const std::uint8_t* p) {
    return (std::uint64_t{be16(p)} << 32) | be32(p + 2);
}
inline std::uint64_t be64(const std::uint8_t* p) {
    return (std::uint64_t{be32(p)} << 32) | be32(p + 4);
}

enum class Side : char { Buy = 'B', Sell = 'S' };

struct Header {               // fields common to every message we decode
    std::uint16_t locate;     // per-symbol index assigned for the trading day
    std::uint64_t timestamp;  // ns since midnight
};

struct AddOrder {             // 'A' (36 bytes) and 'F' (40; the 4-byte MPID
    Header hdr;               // attribution is not needed for book building)
    std::uint64_t ref;
    Side side;
    std::uint32_t shares;
    char stock[9];            // 8 chars space-padded on the wire; trimmed + NUL here
    std::uint32_t price;
};

struct OrderExecuted {        // 'E' (31)
    Header hdr;
    std::uint64_t ref;
    std::uint32_t shares;
    std::uint64_t match;
};

struct OrderExecutedPrice {   // 'C' (36): execution at other than the display price
    Header hdr;
    std::uint64_t ref;
    std::uint32_t shares;
    std::uint64_t match;
    bool printable;
    std::uint32_t price;
};

struct OrderCancel {          // 'X' (23): partial cancel, order stays open
    Header hdr;
    std::uint64_t ref;
    std::uint32_t canceled;
};

struct OrderDelete {          // 'D' (19)
    Header hdr;
    std::uint64_t ref;
};

struct OrderReplace {         // 'U' (35): same side, new ref/price/size,
    Header hdr;               // time priority is lost — modeled as delete+add
    std::uint64_t orig_ref;
    std::uint64_t new_ref;
    std::uint32_t shares;
    std::uint32_t price;
};

// Payload sizes per the ITCH 5.0 spec, excluding the 2-byte length prefix of
// the raw-file framing. Only the types the book consumes are listed; all
// other types are skipped via the frame length, so new/unknown message types
// can never desynchronize the stream.
constexpr int wire_size(char type) {
    switch (type) {
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        default:  return -1;
    }
}

}  // namespace itch
