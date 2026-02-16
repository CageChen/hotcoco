# Tutorial 14: Async File I/O with io_uring

This tutorial covers hotcoco's async file I/O layer built on io_uring. You'll learn how to read and write files without blocking the event loop, using both high-level RAII abstractions and low-level awaitables.

## Table of Contents

1. [Why Async File I/O?](#why-async-file-io)
2. [Architecture Overview](#architecture-overview)
3. [High-Level: The File Class](#high-level-the-file-class)
4. [Positional I/O: ReadAt and WriteAt](#positional-io-readat-and-writeat)
5. [Convenience Methods: ReadAll and WriteAll](#convenience-methods-readall-and-writeall)
6. [File Lifecycle and RAII](#file-lifecycle-and-raii)
7. [Utility Free Functions](#utility-free-functions)
8. [Low-Level Awaitables](#low-level-awaitables)
9. [Error Handling](#error-handling)
10. [Complete Example](#complete-example)

---

## Why Async File I/O?

Traditional `read()` and `write()` syscalls are **synchronous** — they block the calling thread until the kernel completes the operation. In a coroutine-based event loop, this stalls the entire executor:

```
Blocking I/O:
├── co_await SomeNetworkOp()     ← Fast, non-blocking
├── ::read(fd, buf, 4096)        ← BLOCKS entire executor!
├── co_await SomeNetworkOp()     ← Starved while read() blocks
```

With io_uring, file operations are submitted to the kernel's submission queue and complete asynchronously:

```
io_uring File I/O:
├── co_await SomeNetworkOp()     ← Non-blocking
├── co_await file.ReadAt(...)    ← Submits IORING_OP_READ, yields
├── co_await SomeNetworkOp()     ← Runs while read completes
│   ... kernel completes read ...
├── ReadAt resumes               ← Coroutine resumes with result
```

---

## Architecture Overview

The file I/O layer is organized into two levels:

```
┌──────────────────────────────────────────────────────┐
│              User Code (your coroutines)              │
├──────────────────────────────────────────────────────┤
│  High-Level: File class (iouring_file.hpp)           │
│    File::Open, ReadAt, WriteAt, ReadAll, WriteAll    │
│    IoUringUnlink, IoUringMkdir, IoUringStat           │
├──────────────────────────────────────────────────────┤
│  Low-Level: Awaitables (iouring_async_ops.hpp)       │
│    IoUringOpen, IoUringReadFile, IoUringWriteFile     │
│    IoUringClose, IoUringFsync, IoUringStatx           │
│    IoUringUnlinkAt, IoUringMkdirAt                    │
├──────────────────────────────────────────────────────┤
│  IoUringExecutor (io_uring ring + CQ processing)     │
└──────────────────────────────────────────────────────┘
```

Most users should use the **File** class. The low-level awaitables are available for advanced use cases where you need direct control over file descriptors.

---

## High-Level: The File Class

### Opening a File

Files are opened via the static factory `File::Open()`, which returns a `Result`:

```cpp
#include <hotcoco/io/iouring_file.hpp>
using namespace hotcoco;

Task<void> Example() {
    // Open for reading
    auto result = co_await File::Open("/tmp/data.txt");
    if (result.IsErr()) {
        // handle error, e.g. file not found
        co_return;
    }
    auto file = std::move(result).Value();

    // Open for read/write, create if missing
    auto rw = co_await File::Open("/tmp/output.bin", O_RDWR | O_CREAT);
    auto file2 = std::move(rw).Value();
}
```

**Parameters:**

| Parameter | Default | Description |
|-----------|---------|-------------|
| `path` | — | File path (string_view) |
| `flags` | `O_RDONLY` | Standard `open(2)` flags |
| `mode` | `0644` | Permission bits (when creating) |

The `flags` parameter accepts any standard `open(2)` flags:

| Flag | Purpose |
|------|---------|
| `O_RDONLY` | Read-only (default) |
| `O_WRONLY` | Write-only |
| `O_RDWR` | Read and write |
| `O_CREAT` | Create if not exists |
| `O_TRUNC` | Truncate existing file |
| `O_APPEND` | Append mode |

---

## Positional I/O: ReadAt and WriteAt

The File class uses **positional I/O** exclusively — every read/write takes an explicit offset. This is a deliberate design choice:

> **Why not regular read/write?** Traditional `read()`/`write()` use a shared file offset that the kernel advances on each call. With concurrent coroutines sharing the same file, this leads to data races. Positional I/O (`pread`/`pwrite`) avoids the problem entirely.

### ReadAt

```cpp
Task<void> ReadExample(File& file) {
    char buf[1024];

    // Read up to 1024 bytes starting at offset 0
    int32_t bytes_read = co_await file.ReadAt(buf, sizeof(buf), 0);

    if (bytes_read > 0) {
        // Successfully read `bytes_read` bytes
        std::string_view data(buf, bytes_read);
    } else if (bytes_read == 0) {
        // EOF — no more data
    } else {
        // Error: bytes_read is -errno
        // e.g. bytes_read == -EBADF means bad file descriptor
    }
}
```

### WriteAt

```cpp
Task<void> WriteExample(File& file) {
    const char* data = "Hello, io_uring!";
    size_t len = strlen(data);

    // Write at offset 0
    int32_t bytes_written = co_await file.WriteAt(data, len, 0);

    if (bytes_written > 0) {
        // Successfully wrote `bytes_written` bytes
        // Note: may be a short write (bytes_written < len)
    } else {
        // Error: bytes_written is -errno
    }
}
```

**Important**: Both `ReadAt` and `WriteAt` may return fewer bytes than requested (short reads/writes). For guaranteed complete I/O, use `ReadAll`/`WriteAll`.

---

## Convenience Methods: ReadAll and WriteAll

### ReadAll

Reads the entire file into a `std::vector<char>`:

```cpp
Task<void> ReadEntireFile(File& file) {
    auto result = co_await file.ReadAll();
    if (result.IsErr()) {
        // Error reading file
        co_return;
    }

    auto& data = result.Value();
    std::string content(data.begin(), data.end());
    std::cout << "File contents (" << data.size() << " bytes): "
              << content << std::endl;
}
```

Internally, `ReadAll` uses `IoUringStatx` with `AT_EMPTY_PATH` to determine the file size, then loops over `IoUringReadFile` calls until all bytes are read. It handles short reads and early EOF automatically.

### WriteAll

Writes all data, automatically retrying on short writes:

```cpp
Task<void> WriteEntireBuffer(File& file) {
    std::string data = "A complete message that must be fully written.";

    auto result = co_await file.WriteAll(data.data(), data.size(), 0);
    if (result.IsErr()) {
        // Error — even partial writes are reported as errors
        co_return;
    }

    size_t total = result.Value();
    // total == data.size() guaranteed on success
}
```

`WriteAll` loops until all bytes are written. If any individual write returns an error (including zero bytes written), the entire operation fails — partial writes are never reported as success.

---

## File Lifecycle and RAII

### Explicit Close

For deterministic async cleanup, call `Close()` explicitly:

```cpp
Task<void> ProperLifecycle() {
    auto result = co_await File::Open("/tmp/data.bin", O_RDWR | O_CREAT);
    auto file = std::move(result).Value();

    co_await file.WriteAt("data", 4, 0);
    co_await file.Fsync();   // Ensure data reaches disk

    int32_t r = co_await file.Close();  // Async close via io_uring
    // r == 0 on success, negative on error
    // file.IsOpen() is now false
}
```

### Destructor Fallback

If a File is destroyed without calling `Close()`, the destructor calls `::close()` **synchronously**. This is safe but may briefly block the executor:

```cpp
Task<void> ImplicitClose() {
    auto result = co_await File::Open("/tmp/data.bin");
    auto file = std::move(result).Value();
    // ... use file ...
    // ~File() calls ::close() synchronously when file goes out of scope
}
```

> **Best Practice**: Always `co_await file.Close()` when you care about close errors or want to avoid any synchronous blocking.

### Move Semantics

File is move-only. Moving transfers ownership of the file descriptor:

```cpp
Task<void> TransferOwnership() {
    auto result = co_await File::Open("/tmp/data.bin");
    File file = std::move(result).Value();

    File other = std::move(file);
    // file.IsOpen() == false, file.Fd() == -1
    // other.IsOpen() == true
}
```

### Fsync

Flush data and metadata to disk:

```cpp
Task<void> DurableWrite(File& file) {
    co_await file.WriteAt("critical data", 13, 0);
    co_await file.Fsync();  // Full fsync — data + metadata

    // For data-only sync (skip metadata like timestamps):
    co_await file.Fsync(IORING_FSYNC_DATASYNC);
}
```

---

## Utility Free Functions

Three free functions provide common filesystem operations without requiring a File object:

### IoUringMkdir

```cpp
Task<void> CreateDirectory() {
    int32_t r = co_await IoUringMkdir("/tmp/my_app_data");
    // r == 0 on success, -errno on error (e.g. -EEXIST)

    // Custom permissions
    r = co_await IoUringMkdir("/tmp/private_dir", 0700);
}
```

### IoUringStat

```cpp
Task<void> GetFileInfo() {
    auto result = co_await IoUringStat("/tmp/data.bin");
    if (result.IsOk()) {
        auto& st = result.Value();
        std::cout << "Size: " << st.stx_size << " bytes" << std::endl;
        std::cout << "Mode: " << std::oct << st.stx_mode << std::endl;
    }
}
```

### IoUringUnlink

```cpp
Task<void> DeleteFile() {
    int32_t r = co_await IoUringUnlink("/tmp/data.bin");
    // r == 0 on success, -errno on error (e.g. -ENOENT)
}
```

---

## Low-Level Awaitables

For advanced use cases, `iouring_async_ops.hpp` provides 1:1 mappings to `io_uring_prep_*` calls. Each returns an awaitable that yields `int32_t` (the CQE result):

| Awaitable | io_uring Op | Description |
|-----------|-------------|-------------|
| `IoUringOpen(path, flags, mode)` | `IORING_OP_OPENAT` | Open file, returns fd |
| `IoUringOpenAt(dirfd, path, flags, mode)` | `IORING_OP_OPENAT` | Open relative to dirfd |
| `IoUringReadFile(fd, buf, nbytes, offset)` | `IORING_OP_READ` | Positional read |
| `IoUringWriteFile(fd, buf, nbytes, offset)` | `IORING_OP_WRITE` | Positional write |
| `IoUringClose(fd)` | `IORING_OP_CLOSE` | Close file descriptor |
| `IoUringFsync(fd, flags)` | `IORING_OP_FSYNC` | Sync to disk |
| `IoUringStatx(dirfd, path, flags, mask, buf)` | `IORING_OP_STATX` | Extended stat |
| `IoUringUnlinkAt(dirfd, path, flags)` | `IORING_OP_UNLINKAT` | Delete file |
| `IoUringMkdirAt(dirfd, path, mode)` | `IORING_OP_MKDIRAT` | Create directory |

### Example: Manual fd Management

```cpp
Task<void> LowLevelExample() {
    // Open
    int32_t fd = co_await IoUringOpen("/tmp/test.bin", O_RDWR | O_CREAT);
    if (fd < 0) { /* -errno */ co_return; }

    // Write
    const char* msg = "raw io_uring";
    int32_t n = co_await IoUringWriteFile(fd, msg, strlen(msg), 0);

    // Read back
    char buf[32];
    n = co_await IoUringReadFile(fd, buf, sizeof(buf), 0);

    // Close
    co_await IoUringClose(fd);
}
```

### How Awaitables Work

Each awaitable follows the same pattern internally:

1. **`await_ready()`** returns `false` — always suspends
2. **`await_suspend()`** gets the current `IoUringExecutor` via `GetCurrentExecutor()`, prepares an SQE, and submits it to the io_uring ring
3. **`await_resume()`** returns the CQE result (`int32_t`)

All awaitables are **non-copyable and non-movable** because their `OpContext` address is submitted to the kernel as user data. Moving the awaitable would invalidate the pointer.

If no `IoUringExecutor` is active on the current thread, `await_suspend()` returns `false` with result `-ENXIO`, causing immediate resumption without kernel interaction.

---

## Error Handling

All results follow the POSIX convention: **negative values are `-errno`**.

```cpp
Task<void> ErrorHandling() {
    // File::Open returns Result<File, std::error_code>
    auto result = co_await File::Open("/nonexistent");
    if (result.IsErr()) {
        auto& ec = result.Error();
        // ec.value() == ENOENT (2)
        // ec.message() == "No such file or directory"
    }

    // Low-level awaitables return int32_t directly
    int32_t fd = co_await IoUringOpen("/nonexistent", O_RDONLY);
    if (fd < 0) {
        int err = -fd;  // err == ENOENT
    }
}
```

Common error codes:

| Error | Value | Meaning |
|-------|-------|---------|
| `ENOENT` | 2 | File not found |
| `EACCES` | 13 | Permission denied |
| `EEXIST` | 17 | File/directory already exists |
| `EBADF` | 9 | Bad file descriptor (closed/invalid) |
| `ENXIO` | 6 | No IoUringExecutor on current thread |
| `EAGAIN` | 11 | SQ full (try again) |

---

## Complete Example

A practical example: async log file writer.

```cpp
#include <hotcoco/io/iouring_executor.hpp>
#include <hotcoco/io/iouring_file.hpp>
#include <hotcoco/core/task.hpp>
#include <hotcoco/io/timer.hpp>

#include <cstring>
#include <iostream>
#include <string>

using namespace hotcoco;
using namespace std::chrono_literals;

#ifdef HOTCOCO_HAS_IOURING

Task<void> WriteLog(File& log_file, uint64_t& offset) {
    for (int i = 0; i < 5; i++) {
        std::string entry = "Log entry #" + std::to_string(i) + "\n";

        auto result = co_await log_file.WriteAll(entry.data(), entry.size(), offset);
        if (result.IsErr()) {
            std::cerr << "Write failed: " << result.Error().message() << std::endl;
            co_return;
        }
        offset += result.Value();

        co_await AsyncSleep(50ms);  // Simulate work between writes
    }

    // Ensure all entries reach disk
    co_await log_file.Fsync();
}

Task<void> ReadAndPrint(const std::string& path) {
    auto result = co_await File::Open(path);
    if (result.IsErr()) {
        std::cerr << "Open failed: " << result.Error().message() << std::endl;
        co_return;
    }
    auto file = std::move(result).Value();

    auto content = co_await file.ReadAll();
    if (content.IsOk()) {
        auto& data = content.Value();
        std::cout << "=== Log Contents ===" << std::endl;
        std::cout << std::string_view(data.data(), data.size());
        std::cout << "=== End ===" << std::endl;
    }

    co_await file.Close();
}

Task<void> Run() {
    const std::string path = "/tmp/hotcoco_tutorial_log.txt";

    // Create and write
    auto result = co_await File::Open(path, O_RDWR | O_CREAT | O_TRUNC);
    if (result.IsErr()) {
        std::cerr << "Failed to create log: " << result.Error().message() << std::endl;
        co_return;
    }
    auto log_file = std::move(result).Value();

    uint64_t offset = 0;
    co_await WriteLog(log_file, offset);
    co_await log_file.Close();

    // Read back
    co_await ReadAndPrint(path);

    // Stat
    auto stat = co_await IoUringStat(path);
    if (stat.IsOk()) {
        std::cout << "File size: " << stat.Value().stx_size << " bytes" << std::endl;
    }

    // Cleanup
    co_await IoUringUnlink(path);
}

int main() {
    auto executor = IoUringExecutor::Create().Value();
    auto task = Run();
    executor->Schedule(task.GetHandle());
    executor->Run();
}

#endif
```

---

## Summary

| Concept | Purpose |
|---------|---------|
| `File` | RAII async file handle with positional I/O |
| `File::Open` | Async file open returning `Result<File>` |
| `ReadAt` / `WriteAt` | Positional I/O — no shared offset races |
| `ReadAll` / `WriteAll` | Complete read/write with retry loops |
| `Fsync` | Flush data to disk |
| `IoUringUnlink` / `IoUringMkdir` / `IoUringStat` | Filesystem utilities |
| Low-level awaitables | Direct 1:1 io_uring op mappings |

### Key Takeaways

1. **Use positional I/O** (`ReadAt`/`WriteAt`) to avoid file offset races between coroutines
2. **Use `ReadAll`/`WriteAll`** for guaranteed complete transfers
3. **Always `co_await Close()`** for async cleanup; destructors fall back to sync `::close()`
4. **Low-level awaitables** return raw `int32_t` (negative = `-errno`)
5. **Requires `IoUringExecutor`** — awaitables return `-ENXIO` if no io_uring executor is active

---

## Next Steps

- Combine file I/O with TCP networking for an async HTTP static file server
- Use `WhenAll` to read multiple files concurrently
- Explore `PollingExecutor` for SPDK/RDMA-style I/O patterns
