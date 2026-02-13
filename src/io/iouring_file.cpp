// ============================================================================
// hotcoco/io/iouring_file.cpp - io_uring Async File Operations Implementation
// ============================================================================

#ifdef HOTCOCO_HAS_IOURING

#include "hotcoco/io/iouring_file.hpp"

#include "hotcoco/io/iouring_async_ops.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace hotcoco {

// ============================================================================
// File - Static Factory
// ============================================================================

Task<Result<File, std::error_code>> File::Open(std::string_view path, int flags, mode_t mode) {
    int32_t fd = co_await IoUringOpen(std::string(path), flags, mode);
    if (fd < 0) {
        co_return Err(std::error_code(-fd, std::system_category()));
    }
    co_return Ok(File(fd));
}

// ============================================================================
// File - Move Semantics
// ============================================================================

File::File(File&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        // Close existing fd synchronously — we cannot co_await in a noexcept
        // move-assignment operator. Callers that care about async close should
        // call co_await Close() explicitly before reassigning.
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

File::~File() {
    // Synchronous close — destructor cannot co_await. For deterministic async
    // cleanup, callers should co_await Close() before letting the File drop.
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// ============================================================================
// File - Core I/O Operations
// ============================================================================

Task<int32_t> File::ReadAt(void* buf, size_t len, uint64_t offset) {
    if (fd_ < 0) {
        co_return -EBADF;
    }
    // Clamp to unsigned range — io_uring prep_read accepts unsigned nbytes.
    auto clamped = static_cast<unsigned>(std::min(len, static_cast<size_t>(UINT_MAX)));
    co_return co_await IoUringReadFile(fd_, buf, clamped, offset);
}

Task<int32_t> File::WriteAt(const void* buf, size_t len, uint64_t offset) {
    if (fd_ < 0) {
        co_return -EBADF;
    }
    // Clamp to unsigned range — io_uring prep_write accepts unsigned nbytes.
    auto clamped = static_cast<unsigned>(std::min(len, static_cast<size_t>(UINT_MAX)));
    co_return co_await IoUringWriteFile(fd_, buf, clamped, offset);
}

Task<int32_t> File::Fsync(unsigned flags) {
    if (fd_ < 0) {
        co_return -EBADF;
    }
    co_return co_await IoUringFsync(fd_, flags);
}

Task<int32_t> File::Close() {
    if (fd_ < 0) {
        co_return -EBADF;
    }
    int fd_copy = fd_;
    fd_ = -1;  // Prevent destructor double-close
    co_return co_await IoUringClose(fd_copy);
}

// ============================================================================
// File - Convenience Operations
// ============================================================================

Task<Result<std::vector<char>, std::error_code>> File::ReadAll() {
    if (fd_ < 0) {
        co_return Err(std::error_code(EBADF, std::system_category()));
    }

    // Get file size via statx with AT_EMPTY_PATH (stat the fd itself, no path needed).
    // NOTE: This captures a snapshot of the file size. Concurrent modifications
    // (appends or truncates) between the statx and the read loop may cause the
    // result to be shorter or miss newly appended data.
    struct statx stx{};
    int32_t stat_res = co_await IoUringStatx(fd_, "", AT_EMPTY_PATH, STATX_SIZE, &stx);
    if (stat_res < 0) {
        co_return Err(std::error_code(-stat_res, std::system_category()));
    }

    auto file_size = static_cast<size_t>(stx.stx_size);
    if (file_size == 0) {
        co_return Ok(std::vector<char>{});
    }

    std::vector<char> data(file_size);
    size_t total_read = 0;

    while (total_read < file_size) {
        auto chunk = static_cast<unsigned>(std::min(file_size - total_read, static_cast<size_t>(UINT_MAX)));
        int32_t n = co_await IoUringReadFile(fd_, data.data() + total_read, chunk, total_read);
        if (n == 0) {
            // EOF before expected — truncate and return
            data.resize(total_read);
            break;
        }
        if (n < 0) {
            co_return Err(std::error_code(-n, std::system_category()));
        }
        total_read += static_cast<size_t>(n);
    }

    co_return Ok(std::move(data));
}

Task<Result<size_t, std::error_code>> File::WriteAll(const void* buf, size_t len, uint64_t offset) {
    if (fd_ < 0) {
        co_return Err(std::error_code(EBADF, std::system_category()));
    }

    const auto* ptr = static_cast<const char*>(buf);
    size_t total_written = 0;

    while (total_written < len) {
        auto chunk = static_cast<unsigned>(std::min(len - total_written, static_cast<size_t>(UINT_MAX)));
        int32_t n = co_await IoUringWriteFile(fd_, ptr + total_written, chunk, offset + total_written);
        if (n <= 0) {
            // Always report the error — even if some bytes were already written.
            // Returning Ok(partial) would hide the failure from the caller.
            co_return Err(std::error_code(n == 0 ? EIO : -n, std::system_category()));
        }
        total_written += static_cast<size_t>(n);
    }

    co_return Ok(total_written);
}

// ============================================================================
// Utility Free Functions
// ============================================================================

Task<int32_t> IoUringUnlink(std::string_view path) {
    co_return co_await IoUringUnlinkAt(AT_FDCWD, std::string(path), 0);
}

Task<int32_t> IoUringMkdir(std::string_view path, mode_t mode) {
    co_return co_await IoUringMkdirAt(AT_FDCWD, std::string(path), mode);
}

Task<Result<struct statx, std::error_code>> IoUringStat(std::string_view path) {
    struct statx stx{};
    int32_t res = co_await IoUringStatx(AT_FDCWD, std::string(path), 0, STATX_ALL, &stx);
    if (res < 0) {
        co_return Err(std::error_code(-res, std::system_category()));
    }
    co_return Ok(stx);
}

}  // namespace hotcoco

#endif  // HOTCOCO_HAS_IOURING
