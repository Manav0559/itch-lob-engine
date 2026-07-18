#pragma once
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "itch/messages.hpp"

// Synthetic ITCH 5.0 message encoders. These exist so the parser and book can
// be tested (and the replay binary self-tested) against byte-exact wire
// layouts without shipping multi-gigabyte exchange files. The encoders are
// the mirror image of the decoders on purpose: a round-trip failure localizes
// a layout bug to one side.
namespace itch::encode {

using Msg = std::vector<std::uint8_t>;

inline void be16(Msg& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}
inline void be32(Msg& v, std::uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<std::uint8_t>(x >> s));
}
inline void be48(Msg& v, std::uint64_t x) {
    for (int s = 40; s >= 0; s -= 8) v.push_back(static_cast<std::uint8_t>(x >> s));
}
inline void be64(Msg& v, std::uint64_t x) {
    for (int s = 56; s >= 0; s -= 8) v.push_back(static_cast<std::uint8_t>(x >> s));
}
inline void stock8(Msg& v, std::string_view s) {
    for (std::size_t i = 0; i < 8; ++i)
        v.push_back(i < s.size() ? static_cast<std::uint8_t>(s[i]) : ' ');
}

inline void header(Msg& m, char type, std::uint16_t locate, std::uint64_t ts) {
    m.push_back(static_cast<std::uint8_t>(type));
    be16(m, locate);
    be16(m, 0);  // tracking number: unused by the decoder
    be48(m, ts);
}

inline Msg add_order(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                     Side side, std::uint32_t shares, std::string_view stock,
                     std::uint32_t price) {
    Msg m;
    header(m, 'A', locate, ts);
    be64(m, ref);
    m.push_back(static_cast<std::uint8_t>(side));
    be32(m, shares);
    stock8(m, stock);
    be32(m, price);
    return m;
}

inline Msg executed(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                    std::uint32_t shares, std::uint64_t match) {
    Msg m;
    header(m, 'E', locate, ts);
    be64(m, ref);
    be32(m, shares);
    be64(m, match);
    return m;
}

inline Msg executed_price(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                          std::uint32_t shares, std::uint64_t match,
                          bool printable, std::uint32_t price) {
    Msg m;
    header(m, 'C', locate, ts);
    be64(m, ref);
    be32(m, shares);
    be64(m, match);
    m.push_back(printable ? 'Y' : 'N');
    be32(m, price);
    return m;
}

inline Msg cancel(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                  std::uint32_t canceled) {
    Msg m;
    header(m, 'X', locate, ts);
    be64(m, ref);
    be32(m, canceled);
    return m;
}

inline Msg del(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref) {
    Msg m;
    header(m, 'D', locate, ts);
    be64(m, ref);
    return m;
}

inline Msg replace(std::uint16_t locate, std::uint64_t ts, std::uint64_t orig,
                   std::uint64_t next, std::uint32_t shares, std::uint32_t price) {
    Msg m;
    header(m, 'U', locate, ts);
    be64(m, orig);
    be64(m, next);
    be32(m, shares);
    be32(m, price);
    return m;
}

// Frame messages into the raw-file layout: 2-byte big-endian length prefix
// per message.
inline void frame(std::vector<std::uint8_t>& out, const Msg& m) {
    be16(out, static_cast<std::uint16_t>(m.size()));
    out.insert(out.end(), m.begin(), m.end());
}

inline std::vector<std::uint8_t> stream(std::initializer_list<Msg> msgs) {
    std::vector<std::uint8_t> out;
    for (const Msg& m : msgs) frame(out, m);
    return out;
}

}  // namespace itch::encode
