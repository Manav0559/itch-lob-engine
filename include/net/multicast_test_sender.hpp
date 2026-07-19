#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// Test-only helper (despite the header living under include/, not tests/):
// shared verbatim by src/multicast_sender_main.cpp and
// tests/test_multicast_receiver.cpp so the datagram send loop exists in one
// place rather than being copy-pasted between a demo binary and its test.
namespace net {

// FRAMING DECISION: this demo reuses the same 2-byte-length-prefix framing
// itch::parse_stream already reads from files (see itch/parser.hpp), sending
// each complete framed message as one self-contained UDP datagram, rather
// than implementing NASDAQ's MoldUDP64 session/sequencing protocol (see the
// scope note in include/net/multicast_receiver.hpp). A production feed needs
// MoldUDP64's sequence numbers for gap detection on top of this; this demo
// intentionally assumes no packet loss and no reordering — true on loopback,
// not on a real network.
//
// `framed` must already be in that format (see itch::encode::stream). This
// walks it the same way itch::parse_stream does and sends one frame -> one
// datagram.
inline void send_multicast_stream(const std::string& group, std::uint16_t port,
                                   const std::vector<std::uint8_t>& framed) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "socket failed");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, group.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        throw std::system_error(EINVAL, std::generic_category(),
                                "invalid multicast group address: " + group);
    }

    // Loopback delivery to a socket's own joined multicast group depends on
    // IP_MULTICAST_LOOP; it defaults to enabled on Linux/macOS, but a demo
    // meant to run entirely on loopback sets it explicitly rather than
    // relying on the platform default.
    const unsigned char loop_enabled = 1;
    ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop_enabled, sizeof(loop_enabled));

    std::size_t off = 0;
    while (off + 2 <= framed.size()) {
        const std::uint16_t mlen =
            static_cast<std::uint16_t>((std::uint16_t{framed[off]} << 8) | framed[off + 1]);
        const std::size_t frame_len = 2 + std::size_t{mlen};
        if (mlen == 0 || off + frame_len > framed.size()) break;

        const ssize_t sent = ::sendto(fd, framed.data() + off, frame_len, 0,
                                      reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        if (sent < 0) {
            const int err = errno;
            ::close(fd);
            throw std::system_error(err, std::generic_category(), "sendto failed");
        }
        off += frame_len;
    }

    ::close(fd);
}

}  // namespace net
