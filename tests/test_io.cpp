#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>
#include <zlib.h>

#include "io/gzip_source.hpp"
#include "io/mmap_source.hpp"
#include "itch/encode.hpp"
#include "itch/parser.hpp"

namespace {

// std::tmpnam is deprecated (security warning under -Werror on both
// Linux and macOS); build a unique path by hand instead.
std::string make_temp_path(const std::string& suffix) {
    static std::atomic<int> counter{0};
    const char* dir = std::getenv("TMPDIR");
    std::string base = (dir != nullptr && *dir != '\0') ? dir : "/tmp";
    if (base.back() != '/') base += '/';
    return base + "itch_lob_engine_test_" + std::to_string(::getpid()) + "_" +
           std::to_string(counter.fetch_add(1)) + suffix;
}

// Same idea as test_parser.cpp's Recorder: capture every decoded event
// verbatim so a run through GzipSource can be checked field-for-field
// against what was encoded, i.e. byte-for-byte through the frame layer.
struct Recorder {
    std::vector<itch::AddOrder> adds;
    std::vector<itch::OrderExecuted> execs;
    std::vector<std::pair<char, std::size_t>> others;

    void on_add(const itch::AddOrder& m) { adds.push_back(m); }
    void on_execute(const itch::OrderExecuted& m) { execs.push_back(m); }
    void on_execute_price(const itch::OrderExecutedPrice&) {}
    void on_cancel(const itch::OrderCancel&) {}
    void on_delete(const itch::OrderDelete&) {}
    void on_replace(const itch::OrderReplace&) {}
    void on_other(char t, std::size_t len) { others.emplace_back(t, len); }
};

// A path under the OS temp dir, unique per test binary run, cleaned up by
// the destructor so failed assertions don't leak files across runs.
class TempFile {
public:
    explicit TempFile(const std::string& suffix) : path_(make_temp_path(suffix)) {}
    ~TempFile() { std::remove(path_.c_str()); }

    const std::string& path() const { return path_; }

    void write(const std::vector<std::uint8_t>& bytes) const {
        std::ofstream f(path_, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }

private:
    std::string path_;
};

// Compresses `raw` into real gzip framing (header + deflate + crc32/size
// trailer) using zlib's deflate() directly — the mirror-image encoder to
// GzipSource's inflate() decoder, same rationale as itch::encode being the
// mirror of the message decoders.
std::vector<std::uint8_t> gzip_compress(const std::vector<std::uint8_t>& raw) {
    z_stream zs{};
    // +16: emit a gzip header/trailer, not raw zlib framing.
    REQUIRE(deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8,
                         Z_DEFAULT_STRATEGY) == Z_OK);

    std::vector<std::uint8_t> out(deflateBound(&zs, raw.size()));
    zs.next_in = const_cast<std::uint8_t*>(raw.data());
    zs.avail_in = static_cast<uInt>(raw.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());

    const int ret = deflate(&zs, Z_FINISH);
    const std::size_t produced = out.size() - zs.avail_out;
    deflateEnd(&zs);
    REQUIRE(ret == Z_STREAM_END);

    out.resize(produced);
    return out;
}

}  // namespace

TEST_CASE("mmap_source maps a file's exact contents") {
    const std::vector<std::uint8_t> want = {1, 2, 3, 4, 250, 251, 252, 0, 0, 9};
    TempFile tmp(".bin");
    tmp.write(want);

    io::MmapSource src(tmp.path());
    REQUIRE(src.size() == want.size());
    CHECK(std::vector<std::uint8_t>(src.data(), src.data() + src.size()) == want);
}

TEST_CASE("mmap_source handles an empty file") {
    TempFile tmp(".bin");
    tmp.write({});

    io::MmapSource src(tmp.path());
    CHECK(src.size() == 0);
}

TEST_CASE("mmap_source throws on a missing file") {
    CHECK_THROWS_AS(io::MmapSource("/does/not/exist/itch-lob-engine-test"),
                    std::system_error);
}

TEST_CASE("gzip_source decompresses and replays a small stream byte-for-byte") {
    using namespace itch::encode;
    const auto raw = stream({
        add_order(1, 100, 1001, itch::Side::Buy, 300, "AAPL", 1'500'000),
        executed(1, 110, 1001, 300, 90001),
    });

    TempFile tmp(".gz");
    tmp.write(gzip_compress(raw));

    io::GzipSource src(tmp.path());
    Recorder r;
    REQUIRE(src.run(r) == 2);
    REQUIRE(src.bytes_decompressed() == raw.size());

    REQUIRE(r.adds.size() == 1);
    CHECK(r.adds[0].hdr.locate == 1);
    CHECK(r.adds[0].ref == 1001);
    CHECK(r.adds[0].shares == 300);
    CHECK(r.adds[0].price == 1'500'000);

    REQUIRE(r.execs.size() == 1);
    CHECK(r.execs[0].ref == 1001);
    CHECK(r.execs[0].shares == 300);
    CHECK(r.execs[0].match == 90001);
}

TEST_CASE("gzip_source carries a length prefix split across a chunk boundary") {
    using namespace itch::encode;
    const Msg first = add_order(1, 100, 1, itch::Side::Buy, 10, "IBM", 1'000'000);
    const auto raw = stream({first, executed(1, 200, 1, 10, 555)});

    // first's frame is 2 (length prefix) + 36 (payload) = 38 bytes. A
    // decompression chunk size of 39 lands one byte into the *next*
    // message's 2-byte length prefix, splitting the prefix itself.
    TempFile tmp(".gz");
    tmp.write(gzip_compress(raw));
    io::GzipSource src(tmp.path(), /*chunk_size=*/39);

    Recorder r;
    REQUIRE(src.run(r) == 2);
    REQUIRE(r.adds.size() == 1);
    REQUIRE(r.execs.size() == 1);
    CHECK(r.adds[0].ref == 1);
    CHECK(r.execs[0].match == 555);
}

TEST_CASE("gzip_source carries a message payload split across a chunk boundary") {
    using namespace itch::encode;
    const auto raw = stream({
        add_order(1, 100, 1, itch::Side::Buy, 10, "IBM", 1'000'000),
        add_order(1, 200, 2, itch::Side::Sell, 20, "IBM", 1'000'500),
    });

    // 20 bytes lands in the middle of the first message's 36-byte payload
    // (after its 2-byte length prefix), splitting the payload itself.
    TempFile tmp(".gz");
    tmp.write(gzip_compress(raw));
    io::GzipSource src(tmp.path(), /*chunk_size=*/20);

    Recorder r;
    REQUIRE(src.run(r) == 2);
    REQUIRE(r.adds.size() == 2);
    CHECK(r.adds[0].ref == 1);
    CHECK(r.adds[0].shares == 10);
    CHECK(r.adds[1].ref == 2);
    CHECK(r.adds[1].shares == 20);
}

TEST_CASE("gzip_source rejects a truncated gzip file") {
    using namespace itch::encode;
    const auto raw = stream({add_order(1, 100, 1, itch::Side::Buy, 10, "IBM", 1'000'000)});
    auto compressed = gzip_compress(raw);
    compressed.resize(compressed.size() / 2);  // cut off before the gzip trailer

    TempFile tmp(".gz");
    tmp.write(compressed);
    io::GzipSource src(tmp.path());

    Recorder r;
    CHECK_THROWS_AS(src.run(r), std::runtime_error);
}
