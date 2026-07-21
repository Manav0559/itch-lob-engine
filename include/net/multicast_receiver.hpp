#pragma once
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <sys/types.h>
#include <system_error>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "itch/parser.hpp"
#include "net/moldudp64.hpp"

namespace net {

// RAII UDP socket joined to a multicast group. A thin, protocol-agnostic
// primitive: socket setup, group join, and a receive() call. MoldUdp64Receiver
// (below) layers real NASDAQ MoldUDP64 session semantics — sequence
// numbers, gap detection, retransmission requests, heartbeats — on top of
// this; this class by itself makes no assumption about what's inside a
// datagram.
//
// This is a system-call boundary the same way io::MmapSource's construction
// is (see include/io/mmap_source.hpp): socket/bind/setsockopt can all fail
// for reasons outside the program's control, so construction throws
// std::system_error on any failure.
class MulticastReceiver {
public:
    // `recv_timeout` defaults to zero, meaning receive() blocks indefinitely
    // (the documented behavior). A caller that needs to periodically re-check
    // a stop condition (e.g. a Ctrl-C flag) should pass a nonzero timeout
    // rather than relying on signal delivery to interrupt a blocking recv():
    // whether an interrupted blocking syscall returns EINTR or is silently
    // auto-restarted is platform-dependent (macOS's libc signal() restarts
    // system calls by default, glibc's does not for SA_RESTART-less signal()
    // registration on some configurations either) — a receive timeout is the
    // portable way to get a live loop to notice a stop flag on both.
    MulticastReceiver(const std::string& group, std::uint16_t port,
                      std::chrono::milliseconds recv_timeout = std::chrono::milliseconds::zero()) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0)
            throw std::system_error(errno, std::generic_category(), "socket failed");

        // Lets a rerun bind the same port immediately after the previous
        // process released it, instead of waiting out TIME_WAIT.
        const int reuse = 1;
        if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
            const int err = errno;
            ::close(fd_);
            throw std::system_error(err, std::generic_category(),
                                    "setsockopt(SO_REUSEADDR) failed");
        }

        // Multicast receivers bind to the wildcard address, not the group
        // address: the group membership (joined below) is what actually
        // selects which multicast traffic reaches this socket.
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            const int err = errno;
            ::close(fd_);
            throw std::system_error(err, std::generic_category(),
                                    "bind failed: port " + std::to_string(port));
        }

        ip_mreq mreq{};
        if (::inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr) != 1) {
            ::close(fd_);
            throw std::system_error(EINVAL, std::generic_category(),
                                    "invalid multicast group address: " + group);
        }
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
            const int err = errno;
            ::close(fd_);
            throw std::system_error(err, std::generic_category(),
                                    "IP_ADD_MEMBERSHIP failed for group " + group);
        }

        if (recv_timeout > std::chrono::milliseconds::zero()) {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(recv_timeout);
            timeval tv{};
            tv.tv_sec = static_cast<time_t>(us.count() / 1'000'000);
            tv.tv_usec = static_cast<suseconds_t>(us.count() % 1'000'000);
            if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
                const int err = errno;
                ::close(fd_);
                throw std::system_error(err, std::generic_category(),
                                        "setsockopt(SO_RCVTIMEO) failed");
            }
        }
    }

    ~MulticastReceiver() {
        if (fd_ >= 0) ::close(fd_);
    }

    MulticastReceiver(const MulticastReceiver&) = delete;
    MulticastReceiver& operator=(const MulticastReceiver&) = delete;

    MulticastReceiver(MulticastReceiver&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    MulticastReceiver& operator=(MulticastReceiver&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    // Thin wrapper over recv(): blocking (or timing out, if recv_timeout was
    // set), returns bytes received or -1 on error/timeout (errno is EAGAIN /
    // EWOULDBLOCK on a timeout, EINTR on an interrupted call). Unlike
    // construction, a single failed recv() in a live receive loop is not a
    // program-ending condition the way a bad bind/join is — the caller
    // decides whether to retry, log, or stop.
    ssize_t receive(std::uint8_t* buf, std::size_t max_len) {
        return ::recv(fd_, buf, max_len, 0);
    }

private:
    int fd_ = -1;
};

// Real MoldUDP64 session layer on top of MulticastReceiver: sequence-gap
// detection plus a basic gap-fill mechanism, per NASDAQ's public MoldUDP64
// spec.
//
// Two channels, matching the spec's split between them:
//   - the data channel: the joined multicast group, delivering the live
//     feed as a sequence of MoldUDP64 packets (session header + message
//     blocks, or a heartbeat/end-of-session sentinel).
//   - the request channel: a plain unicast UDP socket used to send
//     retransmission requests to the sender's request port and receive its
//     replies — the standard MoldUDP64 request/response pattern (the
//     reply's destination is whatever ephemeral port the request was sent
//     from, the normal connectionless-UDP request/reply idiom, so this
//     socket needs no fixed local port of its own).
//
// Gap handling: `poll()` decodes one data-channel packet and buffers it
// (keyed by sequence number) rather than assuming packets arrive in order.
// After every packet — from either channel — it drains any run of buffered
// packets starting at the next expected sequence number straight into
// itch::parse_stream, so the itch::parse_stream / BookBuilder integration
// downstream is untouched: this class only decides *when* a byte buffer is
// safe to hand to parse_stream, never how it's parsed.
//
// SIMPLIFICATIONS versus the full spec (documented, not hidden): messages
// are requested/replayed one packet per sequence number rather than
// re-batched; a bounded number of gap-fill round trips are attempted before
// giving up on a given gap (a real feed handler would also fall back to a
// snapshot/refresh mechanism, which is out of scope here); and there is no
// separate heartbeat *generation* on this side (only handling of heartbeats
// the sender emits) since this is a receiver.
class MoldUdp64Receiver {
public:
    // `session` must match the sender's session id exactly — packets
    // belonging to a different session are ignored (the spec allows a
    // group to be reused across trading sessions; a receiver joined
    // mid-transition should not splice two sessions' sequence numbers
    // together).
    MoldUdp64Receiver(const std::string& group, std::uint16_t data_port,
                      const std::string& request_host, std::uint16_t request_port,
                      moldudp64::SessionId session,
                      std::chrono::milliseconds data_recv_timeout = std::chrono::milliseconds(200),
                      std::chrono::milliseconds gap_fill_timeout = std::chrono::milliseconds(200))
        : data_(group, data_port, data_recv_timeout),
          session_(session),
          gap_fill_timeout_(gap_fill_timeout) {
        request_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (request_fd_ < 0)
            throw std::system_error(errno, std::generic_category(), "socket failed (request channel)");

        request_addr_.sin_family = AF_INET;
        request_addr_.sin_port = htons(request_port);
        if (::inet_pton(AF_INET, request_host.c_str(), &request_addr_.sin_addr) != 1) {
            const int err = errno;
            ::close(request_fd_);
            throw std::system_error(err, std::generic_category(),
                                    "invalid request-channel host: " + request_host);
        }
        moldudp64::set_recv_timeout(request_fd_, gap_fill_timeout_);
    }

    ~MoldUdp64Receiver() {
        if (request_fd_ >= 0) ::close(request_fd_);
    }

    MoldUdp64Receiver(const MoldUdp64Receiver&) = delete;
    MoldUdp64Receiver& operator=(const MoldUdp64Receiver&) = delete;

    // Reads and processes exactly one datagram from the data channel:
    // returns the number of itch frames dispatched to `handler` as a
    // result (0 for a timeout/error, a heartbeat, a duplicate, or a packet
    // that's still buffered pending a gap-fill). A caller loops this the
    // same way it would loop MulticastReceiver::receive() directly.
    template <typename Handler>
    std::size_t poll(Handler& handler) {
        std::uint8_t buf[65536];
        const ssize_t n = data_.receive(buf, sizeof(buf));
        if (n < static_cast<ssize_t>(moldudp64::kHeaderLen)) return 0;
        return on_packet(buf, static_cast<std::size_t>(n), handler);
    }

    std::uint64_t next_seq() const { return next_seq_; }
    bool session_ended() const { return ended_; }
    std::uint64_t gap_fill_requests_sent() const { return requests_sent_; }
    std::size_t pending_count() const { return pending_.size(); }

private:
    struct PendingPacket {
        std::uint16_t count;
        std::vector<std::uint8_t> payload;  // concatenated message blocks (an itch::parse_stream buffer)
    };

    // Sequence numbers start at 1 per the spec; 0 means "nothing applied yet".
    static constexpr std::uint64_t kFirstSeq = 1;
    static constexpr int kMaxGapFillAttempts = 5;

    template <typename Handler>
    std::size_t on_packet(const std::uint8_t* buf, std::size_t n, Handler& handler) {
        const moldudp64::Header hdr = moldudp64::decode(buf);
        if (!moldudp64::same_session(hdr.session, session_)) return 0;

        if (hdr.count == moldudp64::kEndOfSession) {
            ended_ = true;
            return 0;
        }
        if (hdr.count == moldudp64::kHeartbeat) return 0;  // no message blocks in this packet

        return accept(hdr.seq, hdr.count, buf + moldudp64::kHeaderLen,
                      n - moldudp64::kHeaderLen, handler);
    }

    // Buffers one packet's message blocks (keyed by its starting sequence
    // number — duplicates of an already-buffered or already-applied
    // sequence number are dropped), then attempts to close any open gap and
    // drains whatever is now contiguous with next_seq_.
    template <typename Handler>
    std::size_t accept(std::uint64_t seq, std::uint16_t count, const std::uint8_t* payload,
                       std::size_t len, Handler& handler) {
        if (seq < next_seq_) return 0;  // already applied (duplicate/late retransmit) — ignore
        pending_.emplace(seq, PendingPacket{count, std::vector<std::uint8_t>(payload, payload + len)});

        resolve_gaps();
        return drain(handler);
    }

    // While the earliest buffered packet is still ahead of next_seq_ (a
    // gap), request the missing range and wait (bounded) for a reply on the
    // request channel, buffering whatever comes back the same way a
    // data-channel packet is buffered. Gives up after kMaxGapFillAttempts
    // requests, leaving the gap for a later packet/poll to retry.
    void resolve_gaps() {
        int attempts = 0;
        while (!pending_.empty() && pending_.begin()->first > next_seq_ &&
               attempts < kMaxGapFillAttempts) {
            const std::uint64_t gap_start = next_seq_;
            const std::uint64_t missing = pending_.begin()->first - next_seq_;
            const std::uint16_t request_count =
                static_cast<std::uint16_t>(std::min<std::uint64_t>(missing, 0xFFFEull));

            send_request(gap_start, request_count);
            ++requests_sent_;
            ++attempts;

            std::uint8_t buf[65536];
            const auto deadline = std::chrono::steady_clock::now() + gap_fill_timeout_;
            while (std::chrono::steady_clock::now() < deadline) {
                const ssize_t n = ::recv(request_fd_, buf, sizeof(buf), 0);
                if (n < static_cast<ssize_t>(moldudp64::kHeaderLen)) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // this attempt timed out
                    continue;                                            // EINTR or a short read — retry
                }
                const moldudp64::Header h = moldudp64::decode(buf);
                if (!moldudp64::same_session(h.session, session_)) continue;
                if (h.count == moldudp64::kHeartbeat || h.count == moldudp64::kEndOfSession) continue;
                if (h.seq < next_seq_) continue;  // stale reply — already applied

                pending_.emplace(h.seq, PendingPacket{h.count,
                                 std::vector<std::uint8_t>(buf + moldudp64::kHeaderLen,
                                                            buf + n)});
                if (pending_.begin()->first == next_seq_) break;  // this gap is closed — re-check outer loop
            }
        }
    }

    template <typename Handler>
    std::size_t drain(Handler& handler) {
        std::size_t frames = 0;
        while (!pending_.empty() && pending_.begin()->first == next_seq_) {
            auto node = pending_.begin();
            frames += itch::parse_stream(node->second.payload.data(), node->second.payload.size(), handler);
            next_seq_ += node->second.count;
            pending_.erase(node);
        }
        return frames;
    }

    void send_request(std::uint64_t seq, std::uint16_t count) {
        std::vector<std::uint8_t> pkt;
        moldudp64::encode(pkt, moldudp64::Header{session_, seq, count});
        ::sendto(request_fd_, pkt.data(), pkt.size(), 0,
                reinterpret_cast<sockaddr*>(&request_addr_), sizeof(request_addr_));
    }

    MulticastReceiver data_;
    moldudp64::SessionId session_;
    std::chrono::milliseconds gap_fill_timeout_;
    int request_fd_ = -1;
    sockaddr_in request_addr_{};

    std::uint64_t next_seq_ = kFirstSeq;
    bool ended_ = false;
    std::uint64_t requests_sent_ = 0;
    std::map<std::uint64_t, PendingPacket> pending_;
};

}  // namespace net
