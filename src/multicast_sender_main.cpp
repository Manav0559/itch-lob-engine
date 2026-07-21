#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "itch/encode.hpp"
#include "net/moldudp64.hpp"
#include "net/multicast_test_sender.hpp"

namespace {

// The same synthetic session as replay_main.cpp's --selftest: two symbols,
// adds through replaces, plus one message type the book doesn't decode. This
// exists purely so live_replay_main (and test_multicast_receiver) have a
// known-good session to receive without a real exchange feed.
std::vector<std::uint8_t> synthetic_session() {
    using namespace itch::encode;
    Msg system_event;  // 'S' (12 bytes) — a real type the book ignores
    header(system_event, 'S', 0, 1);
    system_event.push_back('O');

    return stream({
        system_event,
        add_order(1, 100, 1001, itch::Side::Buy, 300, "AAPL", 1'500'000),
        add_order(1, 110, 1002, itch::Side::Buy, 200, "AAPL", 1'499'900),
        add_order(1, 120, 2001, itch::Side::Sell, 500, "AAPL", 1'500'100),
        add_order(2, 130, 3001, itch::Side::Buy, 1000, "MSFT", 4'200'000),
        executed(1, 140, 1001, 300, 90001),           // fills the whole best bid
        cancel(1, 150, 2001, 100),                    // partial cancel, ask stays
        replace(2, 160, 3001, 3002, 800, 4'199'500),  // MSFT bid moves down
        del(1, 170, 1002),                            // AAPL book now ask-only
    });
}

// Must match live_replay_main.cpp's session id — a MoldUdp64Receiver ignores
// any packet whose session doesn't match its own.
constexpr std::string_view kSessionId = "ITCHDEMO01";

}  // namespace

int main(int argc, char** argv) {
    std::string group = "239.255.0.1";
    std::uint16_t data_port = 12345;
    std::uint16_t request_port = 12346;
    if (argc == 4) {
        group = argv[1];
        data_port = static_cast<std::uint16_t>(std::atoi(argv[2]));
        request_port = static_cast<std::uint16_t>(std::atoi(argv[3]));
    } else if (argc != 1) {
        std::fprintf(stderr, "usage: %s [group data_port request_port]\n", argv[0]);
        std::fprintf(stderr,
                     "  sends one synthetic ITCH session as a real MoldUDP64 session (default\n"
                     "  239.255.0.1:12345, gap-fill request channel on request_port default\n"
                     "  12346) — one message per packet, sequenced, ending in an end-of-session\n"
                     "  packet. Stays up for a few seconds afterward to honor any\n"
                     "  retransmission requests before exiting.\n");
        return 2;
    }

    try {
        net::MoldUdp64Sender sender(group, data_port, request_port,
                                    net::moldudp64::make_session(kSessionId));
        const std::vector<std::uint8_t> framed = synthetic_session();
        sender.load(framed);
        std::printf("sending %zu bytes (synthetic session) to %s:%u (session '%s')\n", framed.size(),
                    group.c_str(), data_port, std::string(kSessionId).c_str());
        sender.send_all();
        std::printf("sent — serving gap-fill requests on port %u for a few seconds before exit\n",
                    request_port);
        std::fflush(stdout);

        // A real MoldUDP64 session server stays up for the life of the
        // session precisely so it can honor retransmission requests; this
        // demo sender is a one-shot process, so it keeps its request
        // channel alive for a short grace period afterward instead of
        // exiting the instant the last datagram is on the wire.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            sender.serve_one_request(std::chrono::milliseconds(200));
        }
        std::printf("done\n");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
