#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "itch/encode.hpp"
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

}  // namespace

int main(int argc, char** argv) {
    std::string group = "239.255.0.1";
    std::uint16_t port = 12345;
    if (argc == 3) {
        group = argv[1];
        port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    } else if (argc != 1) {
        std::fprintf(stderr, "usage: %s [group port]\n", argv[0]);
        std::fprintf(stderr, "  sends one synthetic ITCH session, once, as length-prefixed\n");
        std::fprintf(stderr, "  frames over UDP multicast (default 239.255.0.1:12345).\n");
        return 2;
    }

    try {
        const std::vector<std::uint8_t> framed = synthetic_session();
        std::printf("sending %zu bytes (synthetic session) to %s:%u\n", framed.size(),
                    group.c_str(), port);
        net::send_multicast_stream(group, port, framed);
        std::printf("done\n");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
