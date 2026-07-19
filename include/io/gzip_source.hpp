// Integration note: building this requires find_package(ZLIB REQUIRED) and
// target_link_libraries(<target> PRIVATE ZLIB::ZLIB) in CMakeLists.txt.
#pragma once
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <zlib.h>

#include "itch/parser.hpp"

namespace io {

// Streaming gzip decompression for multi-GB ITCH day files: decompresses in
// fixed-size chunks and dispatches each chunk's complete frames to
// itch::parse_stream, rather than inflating the whole day into memory first.
//
// A message's 2-byte length prefix, or its payload, can straddle a chunk
// boundary — inflate() doesn't know about ITCH framing, it just hands back
// however many bytes fit in the output buffer. So each round we append the
// newly-inflated bytes to `pending` (which already holds the unconsumed tail
// from the previous round), scan `pending` for the longest complete-frame
// prefix, parse only that prefix, and keep the remainder in `pending` for
// next time. parse_stream is therefore only ever called on data starting at
// (and ending on) a real frame boundary.
class GzipSource {
public:
    explicit GzipSource(std::string path, std::size_t chunk_size = 4u * 1024 * 1024)
        : path_(std::move(path)), chunk_size_(chunk_size) {
        file_ = std::fopen(path_.c_str(), "rb");
        if (file_ == nullptr)
            throw std::system_error(errno, std::generic_category(), "open failed: " + path_);
    }

    ~GzipSource() {
        if (file_ != nullptr) std::fclose(file_);
    }

    GzipSource(const GzipSource&) = delete;
    GzipSource& operator=(const GzipSource&) = delete;

    // Decompresses and replays the whole file, dispatching every complete
    // frame to h. Single-use: the underlying file is fully consumed by one
    // call. Returns the total frame count (same contract as
    // itch::parse_stream).
    template <typename Handler>
    std::size_t run(Handler& h) {
        z_stream zs{};
        if (inflateInit2(&zs, MAX_WBITS + 16) != Z_OK)  // +16: expect a gzip header, not raw zlib
            throw std::runtime_error("inflateInit2 failed: " + path_);
        const ZStreamGuard zguard{&zs};

        // The compressed-input read size is independent of chunk_size_ (which
        // bounds decompressed output per inflate() call): compression ratio
        // is unpredictable, so tying them together would make the *intended*
        // knob (memory used for decompressed data) hard to reason about.
        std::vector<std::uint8_t> in(kReadSize);
        std::vector<std::uint8_t> out(chunk_size_);
        std::vector<std::uint8_t> pending;  // carry-over tail + freshly inflated bytes
        std::size_t frames = 0;
        int zret = Z_OK;

        do {
            zs.avail_in = static_cast<uInt>(std::fread(in.data(), 1, in.size(), file_));
            if (std::ferror(file_)) throw std::runtime_error("read failed: " + path_);
            if (zs.avail_in == 0) break;  // EOF; truncation is caught below via zret
            zs.next_in = in.data();

            do {
                zs.next_out = out.data();
                zs.avail_out = static_cast<uInt>(out.size());
                zret = inflate(&zs, Z_NO_FLUSH);
                if (zret == Z_NEED_DICT || zret == Z_DATA_ERROR || zret == Z_MEM_ERROR)
                    throw std::runtime_error(std::string("inflate failed: ") +
                                             (zs.msg != nullptr ? zs.msg : "unknown") + " (" +
                                             path_ + ")");

                const std::size_t produced = out.size() - zs.avail_out;
                if (produced > 0) {
                    bytes_ += produced;
                    pending.insert(pending.end(), out.begin(), out.begin() + produced);
                    const std::size_t complete = frame_boundary(pending.data(), pending.size());
                    frames += itch::parse_stream(pending.data(), complete, h);
                    pending.erase(pending.begin(), pending.begin() + complete);
                }
            } while (zs.avail_out == 0);
        } while (zret != Z_STREAM_END);

        if (zret != Z_STREAM_END)
            throw std::runtime_error("gzip stream truncated before end marker: " + path_);
        if (!pending.empty())
            throw std::runtime_error("trailing partial ITCH frame after decompression: " + path_);

        return frames;
    }

    // Total decompressed bytes seen by run(). Only meaningful after run()
    // has completed.
    std::size_t bytes_decompressed() const { return bytes_; }

private:
    static constexpr std::size_t kReadSize = 64u * 1024;

    struct ZStreamGuard {
        z_stream* zs;
        ~ZStreamGuard() { inflateEnd(zs); }
    };

    // Longest prefix of [data, data+len) that consists entirely of complete
    // ITCH frames — the same length-prefix walk as itch::parse_stream, kept
    // separate because parse_stream reports frame *count*, not bytes
    // consumed, and this class needs the latter to know what to carry over.
    static std::size_t frame_boundary(const std::uint8_t* data, std::size_t len) {
        std::size_t off = 0;
        while (off + 2 <= len) {
            const std::uint16_t mlen = itch::be16(data + off);
            if (mlen == 0 || off + 2 + mlen > len) break;
            off += 2 + std::size_t{mlen};
        }
        return off;
    }

    std::string path_;
    std::size_t chunk_size_;
    std::FILE* file_ = nullptr;
    std::size_t bytes_ = 0;
};

}  // namespace io
