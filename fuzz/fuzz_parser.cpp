// libFuzzer harness for itch::parse_stream.
//
// This is the one place in the engine where untrusted bytes (a file on disk,
// or in principle a live multicast feed) become structured data, so the
// harness feeds raw fuzzer input straight through the same path replay_main
// drives from a real file: parse_stream -> dispatch -> BookBuilder -> a real
// book::OrderBook per locate. Fuzzing decode alone would miss bugs that only
// surface once a malformed-but-decodable message sequence reaches the book
// (an order-id collision, a replace whose original ref lives on the wrong
// side, an execute that outlives its order) — BookBuilder is the same struct
// replay_main.cpp and replay_threaded_main.cpp both bottom out in, reused
// as-is rather than reimplemented here.
#include <cstddef>
#include <cstdint>

#include "itch/parser.hpp"
#include "pipeline/dispatch_to_book.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // Defaults to book::LadderBook, same as production replay/replay_threaded
    // now do — this harness fuzzes the path real files actually take.
    pipeline::BookBuilder<> h;
    itch::parse_stream(data, size, h);
    return 0;
}
