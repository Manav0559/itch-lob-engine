#pragma once
#include <cstddef>

#include "itch/messages.hpp"

namespace itch {

namespace detail {

inline Header header(const std::uint8_t* p) {
    // layout: type(1) locate(2) tracking(2) timestamp(6); tracking is a
    // diagnostic field the exchange uses internally — not decoded.
    return Header{be16(p + 1), be48(p + 5)};
}

inline void copy_stock(char (&dst)[9], const std::uint8_t* src) {
    std::memcpy(dst, src, 8);
    dst[8] = '\0';
    for (int i = 7; i >= 0 && dst[i] == ' '; --i) dst[i] = '\0';
}

}  // namespace detail

// Decode one framed message and invoke the matching handler callback.
// Handler interface: on_add, on_execute, on_execute_price, on_cancel,
// on_delete, on_replace, on_other(type, len).
// Returns false for types we do not decode OR for a known type whose length
// does not match the spec (corrupt frame) — both routed to on_other so the
// caller can count them; the stream itself stays in sync via frame lengths.
template <typename Handler>
bool dispatch(const std::uint8_t* p, std::size_t len, Handler& h) {
    const char type = static_cast<char>(p[0]);
    const int expect = wire_size(type);
    if (expect < 0 || static_cast<std::size_t>(expect) != len) {
        h.on_other(type, len);
        return false;
    }
    switch (type) {
        case 'A':
        case 'F': {
            AddOrder m;
            m.hdr = detail::header(p);
            m.ref = be64(p + 11);
            m.side = static_cast<Side>(p[19]);
            m.shares = be32(p + 20);
            detail::copy_stock(m.stock, p + 24);
            m.price = be32(p + 32);
            h.on_add(m);
            break;
        }
        case 'E': {
            OrderExecuted m;
            m.hdr = detail::header(p);
            m.ref = be64(p + 11);
            m.shares = be32(p + 19);
            m.match = be64(p + 23);
            h.on_execute(m);
            break;
        }
        case 'C': {
            OrderExecutedPrice m;
            m.hdr = detail::header(p);
            m.ref = be64(p + 11);
            m.shares = be32(p + 19);
            m.match = be64(p + 23);
            m.printable = (p[31] == 'Y');
            m.price = be32(p + 32);
            h.on_execute_price(m);
            break;
        }
        case 'X': {
            OrderCancel m;
            m.hdr = detail::header(p);
            m.ref = be64(p + 11);
            m.canceled = be32(p + 19);
            h.on_cancel(m);
            break;
        }
        case 'D': {
            OrderDelete m;
            m.hdr = detail::header(p);
            m.ref = be64(p + 11);
            h.on_delete(m);
            break;
        }
        case 'U': {
            OrderReplace m;
            m.hdr = detail::header(p);
            m.orig_ref = be64(p + 11);
            m.new_ref = be64(p + 19);
            m.shares = be32(p + 27);
            m.price = be32(p + 31);
            h.on_replace(m);
            break;
        }
    }
    return true;
}

// Walk a raw NASDAQ "BinaryFILE" buffer: each message is preceded by a
// 2-byte big-endian payload length. A truncated tail (partial length prefix
// or partial payload) terminates the walk cleanly rather than reading past
// the buffer. Returns the number of complete frames consumed.
template <typename Handler>
std::size_t parse_stream(const std::uint8_t* data, std::size_t len, Handler& h) {
    std::size_t off = 0;
    std::size_t count = 0;
    while (off + 2 <= len) {
        const std::uint16_t mlen = be16(data + off);
        if (mlen == 0 || off + 2 + mlen > len) break;
        dispatch(data + off + 2, mlen, h);
        off += 2 + std::size_t{mlen};
        ++count;
    }
    return count;
}

}  // namespace itch
