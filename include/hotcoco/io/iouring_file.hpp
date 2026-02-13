// ============================================================================
// hotcoco/io/iouring_file.hpp - io_uring Async File Operations
// ============================================================================
//
// High-level file abstraction using io_uring for truly async file I/O.
// Uses positional reads/writes (ReadAt/WriteAt) to avoid shared file
// position races. Internally delegates to IoUringXxx() free functions
// from iouring_async_ops.hpp.
//
// USAGE:
// ------
//   Task<void> Example() {
//       auto result = co_await File::Open("/tmp/data.bin", O_RDWR | O_CREAT);
//       if (result.IsErr()) { /* handle error */ }
//       auto file = std::move(result).Value();
//
//       co_await file.WriteAt("Hello", 5, 0);
//       char buf[5];
//       co_await file.ReadAt(buf, 5, 0);
//       co_await file.Close();
//   }
//
//   // Utility free functions
//   co_await IoUringMkdir("/tmp/mydir");
//   auto stat = co_await IoUringStat("/tmp/mydir");
//   co_await IoUringUnlink("/tmp/data.bin");
//
// ============================================================================

#pragma once

#ifdef HOTCOCO_HAS_IOURING

#include "hotcoco/core/result.hpp"
#include "hotcoco/core/task.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <string_view>
#include <system_error>
#include <vector>

namespace hotcoco {

// ============================================================================
// File - RAII Async File Handle
// ============================================================================
class File {
   public:
    // ========================================================================
    // Static Factory
    // ========================================================================

    /// Asynchronously opens a file. Returns Result with the File on success.
    [[nodiscard]] static Task<Result<File, std::error_code>> Open(std::string_view path, int flags = O_RDONLY,
                                                                  mode_t mode = 0644);

    // ========================================================================
    // Move-only semantics
    // ========================================================================
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    ~File();

    // Non-copyable
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    // ========================================================================
    // Core I/O Operations
    // ========================================================================

    /// Positional read: reads up to len bytes at the given offset.
    /// Returns bytes read (>0), 0 on EOF, or negative error.
    [[nodiscard]] Task<int32_t> ReadAt(void* buf, size_t len, uint64_t offset);

    /// Positional write: writes up to len bytes at the given offset.
    /// Returns bytes written (>0) or negative error.
    [[nodiscard]] Task<int32_t> WriteAt(const void* buf, size_t len, uint64_t offset);

    /// Sync file data to disk. flags=0 for full sync, IORING_FSYNC_DATASYNC
    /// for data-only sync.
    [[nodiscard]] Task<int32_t> Fsync(unsigned flags = 0);

    /// Async close. The fd is invalidated (set to -1) before the kernel
    /// close completes, so even if close returns a non-zero error the file
    /// is no longer usable. The error code is primarily for diagnostics.
    [[nodiscard]] Task<int32_t> Close();

    // ========================================================================
    // Convenience Operations
    // ========================================================================

    /// Read entire file contents from offset 0 into a vector.
    [[nodiscard]] Task<Result<std::vector<char>, std::error_code>> ReadAll();

    /// Write all data, looping on short writes. Returns total bytes written.
    [[nodiscard]] Task<Result<size_t, std::error_code>> WriteAll(const void* buf, size_t len, uint64_t offset = 0);

    // ========================================================================
    // Queries
    // ========================================================================

    [[nodiscard]] int Fd() const noexcept { return fd_; }
    [[nodiscard]] bool IsOpen() const noexcept { return fd_ >= 0; }

   private:
    explicit File(int fd) : fd_(fd) {}
    int fd_ = -1;
};

// ============================================================================
// Utility Free Functions
// ============================================================================

/// Asynchronously delete a file.
[[nodiscard]] Task<int32_t> IoUringUnlink(std::string_view path);

/// Asynchronously create a directory.
[[nodiscard]] Task<int32_t> IoUringMkdir(std::string_view path, mode_t mode = 0755);

/// Asynchronously stat a file, returns Result with struct statx.
[[nodiscard]] Task<Result<struct statx, std::error_code>> IoUringStat(std::string_view path);

}  // namespace hotcoco

#endif  // HOTCOCO_HAS_IOURING
