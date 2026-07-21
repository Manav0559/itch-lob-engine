#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <sys/socket.h>
#include <sys/time.h>

// Wire format for NASDAQ's public MoldUDP64 session protocol: the session
// header every packet on both the data (multicast) channel and the
// request/retransmission channel starts with, plus the small set of
// sentinel values (heartbeat, end-of-session) the protocol defines around
// it.
//
// Deliberate simplification vs. the full spec: a "message count" field
// wider than 1 (multiple message blocks packed into a single datagram) is
// supported by this encoding (the count is a real field, not hardcoded),
// but net::MoldUdp64Sender always sends one message per packet. Batching
// is a throughput optimization the real protocol allows and production
// feed handlers use; it doesn't change the session/gap-fill semantics
// this implementation targets, so it's left as a possible follow-up
// rather than implemented here.
//
// The payload that follows the header on the data channel — the
// concatenated message blocks — is intentionally byte-identical to the
// 2-byte-length-prefix-per-message framing itch::parse_stream already
// reads from files (see itch/parser.hpp): a MoldUDP64 packet's payload
// *is* an itch::parse_stream buffer. That is what lets
// net::MoldUdp64Receiver hand decoded packets straight to
// itch::parse_stream unchanged.
namespace net::moldudp64 {

inline constexpr std::size_t kSessionLen = 10;
inline constexpr std::size_t kHeaderLen = kSessionLen + 8 + 2;  // session + seq(8) + count(2)

// Message-count sentinels (MoldUDP64 spec, packet format section).
inline constexpr std::uint16_t kHeartbeat = 0;        // packet carries no message blocks
inline constexpr std::uint16_t kEndOfSession = 0xFFFF;

using SessionId = std::array<char, kSessionLen>;

// Builds a fixed-width, space-padded session id the way the spec requires
// (ASCII, left-justified, space-filled) from a shorter human-readable name.
inline SessionId make_session(std::string_view s) {
    SessionId id{};
    id.fill(' ');
    std::memcpy(id.data(), s.data(), std::min(s.size(), kSessionLen));
    return id;
}

inline bool same_session(const SessionId& a, const SessionId& b) {
    return std::memcmp(a.data(), b.data(), kSessionLen) == 0;
}

// The 20-byte header prefixing every packet on both channels. On the data
// channel `count` is the number of message blocks that follow (or one of
// the sentinels above); on the request channel it is reused verbatim as
// "requested message count" (the spec defines the request packet as the
// same {session, sequence, count} triple with that reinterpretation,
// which is why one struct/encode/decode pair serves both).
struct Header {
    SessionId session{};
    std::uint64_t seq = 0;
    std::uint16_t count = 0;
};

inline void put_be64(std::uint8_t* p, std::uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) *p++ = static_cast<std::uint8_t>(v >> s);
}
inline void put_be16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}
inline std::uint64_t get_be64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
inline std::uint16_t get_be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((std::uint16_t{p[0]} << 8) | p[1]);
}

// Appends the encoded 20-byte header to `out` (does not touch any payload
// that follows it — callers append message blocks themselves).
inline void encode(std::vector<std::uint8_t>& out, const Header& h) {
    const std::size_t base = out.size();
    out.resize(base + kHeaderLen);
    std::memcpy(out.data() + base, h.session.data(), kSessionLen);
    put_be64(out.data() + base + kSessionLen, h.seq);
    put_be16(out.data() + base + kSessionLen + 8, h.count);
}

// `p` must point at >= kHeaderLen readable bytes.
inline Header decode(const std::uint8_t* p) {
    Header h;
    std::memcpy(h.session.data(), p, kSessionLen);
    h.seq = get_be64(p + kSessionLen);
    h.count = get_be16(p + kSessionLen + 8);
    return h;
}

// SO_RCVTIMEO is the same portable poll-with-timeout mechanism
// net::MulticastReceiver uses (see its constructor comment for why a
// timeout, not relying on EINTR, is what makes a loop able to notice a
// stop condition across the Linux/macOS matrix this repo targets) — the
// request/retransmission channel needs the identical treatment since
// waiting for a gap-fill reply must not be able to block forever.
inline void set_recv_timeout(int fd, std::chrono::milliseconds timeout) {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(timeout);
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(us.count() / 1'000'000);
    tv.tv_usec = static_cast<suseconds_t>(us.count() % 1'000'000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

}  // namespace net::moldudp64
