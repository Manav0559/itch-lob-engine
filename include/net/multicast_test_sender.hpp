#pragma once
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net/moldudp64.hpp"

// Test/demo helper (despite living under include/, not tests/): shared
// verbatim by src/multicast_sender_main.cpp and
// tests/test_multicast_receiver.cpp so the MoldUDP64 send/retransmit logic
// exists in one place rather than being copy-pasted between a demo binary
// and its tests.
namespace net {

// A real (if minimally-batched — see moldudp64.hpp's header comment)
// MoldUDP64 session sender: frames each message into its own packet behind
// a session header, keeps every sent message in memory so it can honor
// retransmission requests, and can simulate real packet loss for tests via
// a caller-supplied predicate.
class MoldUdp64Sender {
public:
    MoldUdp64Sender(const std::string& group, std::uint16_t data_port,
                    std::uint16_t request_port, moldudp64::SessionId session)
        : session_(session) {
        data_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (data_fd_ < 0) throw std::system_error(errno, std::generic_category(), "socket failed (data channel)");

        data_addr_.sin_family = AF_INET;
        data_addr_.sin_port = htons(data_port);
        if (::inet_pton(AF_INET, group.c_str(), &data_addr_.sin_addr) != 1) {
            const int err = errno;
            ::close(data_fd_);
            throw std::system_error(err, std::generic_category(), "invalid multicast group address: " + group);
        }
        // Same rationale as multicast_test_sender's original loopback note:
        // a demo/test meant to run entirely on loopback sets
        // IP_MULTICAST_LOOP explicitly rather than relying on the platform
        // default.
        const unsigned char loop_enabled = 1;
        ::setsockopt(data_fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop_enabled, sizeof(loop_enabled));

        request_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (request_fd_ < 0) {
            const int err = errno;
            ::close(data_fd_);
            throw std::system_error(err, std::generic_category(), "socket failed (request channel)");
        }
        const int reuse = 1;
        ::setsockopt(request_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(request_port);
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(request_fd_, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
            const int err = errno;
            ::close(data_fd_);
            ::close(request_fd_);
            throw std::system_error(err, std::generic_category(),
                                    "bind failed (request channel): port " + std::to_string(request_port));
        }
    }

    ~MoldUdp64Sender() {
        if (data_fd_ >= 0) ::close(data_fd_);
        if (request_fd_ >= 0) ::close(request_fd_);
    }

    MoldUdp64Sender(const MoldUdp64Sender&) = delete;
    MoldUdp64Sender& operator=(const MoldUdp64Sender&) = delete;

    // Splits `framed` — the same 2-byte-length-prefix-per-message format
    // itch::encode::stream produces — into individual message blocks, one
    // per MoldUDP64 sequence number (sequence numbers are 1-based, per the
    // spec), and keeps them so send_all()/retransmission can refer back to
    // them by sequence number.
    void load(const std::vector<std::uint8_t>& framed) {
        messages_.clear();
        std::size_t off = 0;
        while (off + 2 <= framed.size()) {
            const std::uint16_t mlen =
                static_cast<std::uint16_t>((std::uint16_t{framed[off]} << 8) | framed[off + 1]);
            const std::size_t frame_len = 2 + std::size_t{mlen};
            if (mlen == 0 || off + frame_len > framed.size()) break;
            messages_.emplace_back(framed.begin() + static_cast<long>(off),
                                   framed.begin() + static_cast<long>(off + frame_len));
            off += frame_len;
        }
    }

    // Sends every loaded message once, in sequence order, as its own
    // MoldUDP64 packet (count=1) — except any sequence number for which
    // `should_drop(seq)` returns true, which is recorded as sent (so a
    // later retransmission request for it can still be honored) but never
    // actually put on the wire. That's how tests simulate real packet loss
    // without a real lossy network. Finishes with an end-of-session packet.
    template <typename ShouldDrop>
    void send_all(ShouldDrop should_drop) {
        for (std::size_t i = 0; i < messages_.size(); ++i) {
            const std::uint64_t seq = static_cast<std::uint64_t>(i) + 1;
            if (!should_drop(seq)) send_data_packet(seq, 1, messages_[i]);
        }
        send_end_of_session(static_cast<std::uint64_t>(messages_.size()) + 1);
    }
    void send_all() {
        send_all([](std::uint64_t) { return false; });
    }

    // Sends one data-channel packet with an arbitrary sequence number and
    // payload, bypassing load()/send_all()'s in-order framing entirely —
    // for tests that need to simulate a sender racing far ahead of the
    // receiver, or one emitting corrupted/out-of-range sequence numbers.
    // That's exactly the scenario net::MoldUdp64Receiver's `pending_` size
    // cap exists to bound (see kMaxPendingPackets in
    // include/net/multicast_receiver.hpp), so this is what a test exercising
    // that cap sends.
    void send_raw(std::uint64_t seq, std::uint16_t count, const std::vector<std::uint8_t>& payload = {}) {
        send_data_packet(seq, count, payload);
    }

    // Sends a MoldUDP64 heartbeat: a packet with no message blocks, used by
    // the real protocol to let a receiver confirm the session is still
    // alive (and, implicitly, what sequence number comes next) during a
    // quiet period with nothing to send. `next_seq` is the sequence number
    // of whatever message would be sent next, per the spec's definition of
    // a heartbeat's sequence field.
    void send_heartbeat(std::uint64_t next_seq) {
        std::vector<std::uint8_t> pkt;
        moldudp64::encode(pkt, moldudp64::Header{session_, next_seq, moldudp64::kHeartbeat});
        ::sendto(data_fd_, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*>(&data_addr_),
                sizeof(data_addr_));
    }

    // Services at most one retransmission request arriving within
    // `timeout`: replies by resending the requested sequence range as
    // individual packets, unicast back to whoever sent the request (the
    // standard connectionless-UDP request/reply pattern — no need to know
    // the requester's address ahead of time). Returns false on a timeout
    // (nothing to service).
    bool serve_one_request(std::chrono::milliseconds timeout) {
        moldudp64::set_recv_timeout(request_fd_, timeout);
        std::uint8_t buf[64];
        sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        const ssize_t n =
            ::recvfrom(request_fd_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromlen);
        if (n < static_cast<ssize_t>(moldudp64::kHeaderLen)) return false;

        const moldudp64::Header hdr = moldudp64::decode(buf);
        if (!moldudp64::same_session(hdr.session, session_)) return false;

        for (std::uint32_t k = 0; k < hdr.count; ++k) {
            const std::uint64_t seq = hdr.seq + k;
            if (seq < 1 || seq > messages_.size()) break;
            send_reply_packet(from, fromlen, seq, 1, messages_[seq - 1]);
        }
        return true;
    }

private:
    void send_data_packet(std::uint64_t seq, std::uint16_t count, const std::vector<std::uint8_t>& payload) {
        std::vector<std::uint8_t> pkt;
        moldudp64::encode(pkt, moldudp64::Header{session_, seq, count});
        pkt.insert(pkt.end(), payload.begin(), payload.end());
        ::sendto(data_fd_, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*>(&data_addr_),
                sizeof(data_addr_));
    }

    void send_reply_packet(const sockaddr_in& to, socklen_t tolen, std::uint64_t seq, std::uint16_t count,
                           const std::vector<std::uint8_t>& payload) {
        std::vector<std::uint8_t> pkt;
        moldudp64::encode(pkt, moldudp64::Header{session_, seq, count});
        pkt.insert(pkt.end(), payload.begin(), payload.end());
        ::sendto(request_fd_, pkt.data(), pkt.size(), 0, reinterpret_cast<const sockaddr*>(&to), tolen);
    }

    void send_end_of_session(std::uint64_t final_seq) {
        std::vector<std::uint8_t> pkt;
        moldudp64::encode(pkt, moldudp64::Header{session_, final_seq, moldudp64::kEndOfSession});
        ::sendto(data_fd_, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*>(&data_addr_),
                sizeof(data_addr_));
    }

    moldudp64::SessionId session_;
    int data_fd_ = -1;
    int request_fd_ = -1;
    sockaddr_in data_addr_{};
    std::vector<std::vector<std::uint8_t>> messages_;  // index i == sequence number i+1
};

}  // namespace net
