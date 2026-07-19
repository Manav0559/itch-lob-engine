#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace io {

// RAII mmap of a whole file, read-only. Exposes the same (const uint8_t*,
// size_t) shape itch::parse_stream already takes, so the parser needs no
// changes to consume a mapped file instead of a heap buffer — only
// replay_main's file-loading path differs.
//
// This is a system-call boundary (open/fstat/mmap can all fail for reasons
// outside the program's control — permissions, missing file, exhausted
// mappings), so unlike the parsing hot path, real error handling here is
// correct rather than overkill: every failure throws std::system_error with
// the errno that caused it.
class MmapSource {
public:
    explicit MmapSource(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0)
            throw std::system_error(errno, std::generic_category(), "open failed: " + path);

        struct stat st{};
        if (::fstat(fd, &st) != 0) {
            const int err = errno;
            ::close(fd);
            throw std::system_error(err, std::generic_category(), "fstat failed: " + path);
        }
        size_ = static_cast<std::size_t>(st.st_size);

        if (size_ == 0) {
            // mmap() of a zero-length region is undefined by POSIX; an empty
            // file legitimately has no bytes to map, so data() just returns
            // nullptr and size() returns 0.
            ::close(fd);
            return;
        }

        void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            const int err = errno;
            ::close(fd);
            throw std::system_error(err, std::generic_category(), "mmap failed: " + path);
        }
        data_ = static_cast<const std::uint8_t*>(p);

        // Replay is a single forward scan over the whole file, never
        // revisited — tell the kernel so it can drop pages behind the scan
        // instead of caching them for reuse that will never happen.
        ::madvise(p, size_, MADV_SEQUENTIAL);

        // The fd isn't needed once mapped; the mapping outlives it.
        ::close(fd);
    }

    ~MmapSource() { reset(); }

    MmapSource(const MmapSource&) = delete;
    MmapSource& operator=(const MmapSource&) = delete;

    MmapSource(MmapSource&& other) noexcept : data_(other.data_), size_(other.size_) {
        other.data_ = nullptr;
        other.size_ = 0;
    }
    MmapSource& operator=(MmapSource&& other) noexcept {
        if (this != &other) {
            reset();
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    const std::uint8_t* data() const { return data_; }
    std::size_t size() const { return size_; }

private:
    void reset() {
        if (data_ != nullptr) ::munmap(const_cast<std::uint8_t*>(data_), size_);
        data_ = nullptr;
        size_ = 0;
    }

    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace io
