#pragma once
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <system_error>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace net {

// RAII UDP socket joined to a multicast group.
//
// SCOPE: this is a proof-of-concept of the multicast delivery MECHANISM —
// socket setup, group join, and a receive loop that hands frames to the
// existing itch decoder. Real NASDAQ TotalView-ITCH is delivered live wrapped
// in NASDAQ's MoldUDP64 session protocol, which adds sequence numbers, gap
// detection, retransmission requests, and heartbeats on top of raw UDP
// multicast. MoldUDP64 is a large, well-defined subprotocol in its own right
// and implementing it is explicitly OUT OF SCOPE here. This class — and the
// demo built on it — assumes no packet loss and no reordering, which only
// holds for a same-host loopback demo. A production feed handler needs
// MoldUDP64 (or equivalent) layered on top of this before it could survive
// a real network path.
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

}  // namespace net
