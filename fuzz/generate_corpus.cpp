// One-shot tool that writes fuzz/corpus/*.bin using the same itch::encode
// mirror encoders tests/test_book.cpp and replay_main.cpp's selftest() use,
// so the fuzzer starts from real framed ITCH structure instead of pure
// noise. Not part of the CMake build or the fuzz binary itself — run it
// once (see fuzz/README.md) whenever the corpus needs regenerating; the
// output .bin files are what actually gets committed and fed to libFuzzer.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "itch/encode.hpp"
#include "itch/messages.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;

void write_file(const std::string& path, const Bytes& buf) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "error: cannot open %s for writing\n", path.c_str());
        std::exit(1);
    }
    std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
}

// Byte-for-byte the same session as src/replay_main.cpp's selftest(): two
// symbols, adds through a replace and a delete, plus one type the book
// deliberately doesn't decode ('S'). Kept as a literal copy rather than a
// shared header — it's nine lines of encoder calls, not worth a shared
// dependency between the fuzz corpus and the replay binary.
Bytes selftest_stream() {
    using namespace itch::encode;
    Msg system_event;
    header(system_event, 'S', 0, 1);
    system_event.push_back('O');

    return stream({
        system_event,
        add_order(1, 100, 1001, itch::Side::Buy, 300, "AAPL", 1'500'000),
        add_order(1, 110, 1002, itch::Side::Buy, 200, "AAPL", 1'499'900),
        add_order(1, 120, 2001, itch::Side::Sell, 500, "AAPL", 1'500'100),
        add_order(2, 130, 3001, itch::Side::Buy, 1000, "MSFT", 4'200'000),
        executed(1, 140, 1001, 300, 90001),
        cancel(1, 150, 2001, 100),
        replace(2, 160, 3001, 3002, 800, 4'199'500),
        del(1, 170, 1002),
    });
}

// Hand-built 'F' (attributed add, 40 bytes): encode.hpp has no helper for it
// because no test needed one yet, but dispatch() decodes 'A' and 'F'
// identically aside from frame length — the trailing 4-byte MPID is never
// read. Seeding one here gets 'F' into the corpus's type coverage.
itch::encode::Msg add_order_attributed(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                                       itch::Side side, std::uint32_t shares,
                                       std::string_view stock, std::uint32_t price,
                                       std::string_view mpid) {
    using namespace itch::encode;
    Msg m = add_order(locate, ts, ref, side, shares, stock, price);
    m[0] = 'F';
    for (std::size_t i = 0; i < 4; ++i)
        m.push_back(i < mpid.size() ? static_cast<std::uint8_t>(mpid[i]) : ' ');
    return m;
}

// Every decoded type plus one unknown type ('H', trading halt — real ITCH
// type this engine skips) spread across two locates, so a mutation seeded
// from this entry can plausibly reach any handler branch.
Bytes mixed_types_stream() {
    using namespace itch::encode;
    Msg halt;
    header(halt, 'H', 9, 500);
    for (int i = 0; i < 24; ++i) halt.push_back(0);  // arbitrary payload, length-only

    return stream({
        add_order(4, 10, 5001, itch::Side::Buy, 100, "TSLA", 900'000),
        add_order_attributed(4, 20, 5002, itch::Side::Sell, 150, "TSLA", 900'500, "NITE"),
        executed_price(4, 30, 5001, 40, 900'001, true, 899'900),
        halt,
        cancel(9, 40, 5002, 50),
        add_order(9, 50, 6001, itch::Side::Buy, 75, "MSFT", 4'100'000),
        replace(9, 60, 6001, 6002, 60, 4'100'500),
        del(4, 70, 5002),
    });
}

// A chain of replaces on the same lineage, plus deletes/cancels that fire
// against refs already retired by an earlier replace in the same stream —
// on a real feed a gap produces exactly this shape, and OrderBook's
// mutators are specified to return false rather than misbehave, so this
// exercises that path from the top of the stack down.
Bytes replace_chain_stream() {
    using namespace itch::encode;
    return stream({
        add_order(1, 10, 100, itch::Side::Buy, 1000, "IBM", 1'400'000),
        replace(1, 20, 100, 101, 900, 1'400'100),
        replace(1, 30, 101, 102, 800, 1'400'200),
        replace(1, 40, 102, 103, 700, 1'400'300),
        del(1, 50, 100),    // stale: 100 was retired by the first replace
        cancel(1, 60, 101, 50),  // stale: 101 was retired by the second replace
        executed(1, 70, 103, 200, 77001),
        del(1, 80, 103),
    });
}

// Boundary values a fuzzer would eventually find on its own, seeded up
// front: locate 0 and 0xFFFF, a zero-share add, price 0 and uint32 max,
// and a ref that collides with an already-open order.
Bytes boundary_values_stream() {
    using namespace itch::encode;
    return stream({
        add_order(0, 0, 1, itch::Side::Buy, 0, "A", 0),
        add_order(0xFFFF, 0xFFFF'FFFF'FFFFULL, 0xFFFF'FFFF'FFFF'FFFFULL, itch::Side::Sell,
                  0xFFFF'FFFF, "ZZZZZZZZ", 0xFFFF'FFFF),
        add_order(1, 1, 42, itch::Side::Buy, 10, "DUP", 100),
        add_order(1, 2, 42, itch::Side::Sell, 20, "DUP", 200),  // duplicate ref, opposite side
        executed(0xFFFF, 3, 0xFFFF'FFFF'FFFF'FFFFULL, 0xFFFF'FFFF, 0),
    });
}

}  // namespace

int main() {
    const std::string dir = "fuzz/corpus/";
    write_file(dir + "selftest.bin", selftest_stream());
    write_file(dir + "mixed_types.bin", mixed_types_stream());
    write_file(dir + "replace_chain.bin", replace_chain_stream());
    write_file(dir + "boundary_values.bin", boundary_values_stream());
    std::printf("wrote 4 corpus entries to %s\n", dir.c_str());
    return 0;
}
