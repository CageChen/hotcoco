// ============================================================================
// hotcoco/io/iouring_tcp.hpp - io_uring TCP Networking
// ============================================================================
//
// High-level TCP abstractions using io_uring, symmetric with the libuv-based
// TcpListener/TcpStream. Internally uses IoUringXxx() free functions from
// iouring_async_ops.hpp.
//
// USAGE:
// ------
//   Task<void> Server(IoUringExecutor& executor) {
//       IoUringTcpListener listener(executor);
//       listener.Bind("0.0.0.0", 8080);
//       listener.Listen();
//
//       auto stream = co_await listener.Accept();
//       auto data = co_await stream->Read();
//       co_await stream->Write("Hello!");
//   }
//
// ============================================================================

#pragma once

#ifdef HOTCOCO_HAS_IOURING

#include "hotcoco/core/task.hpp"
#include "hotcoco/io/iouring_executor.hpp"

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hotcoco {

// Forward declaration
class IoUringTcpStream;

// ============================================================================
// IoUringTcpListener - Accept Incoming Connections via io_uring
// ============================================================================
class IoUringTcpListener {
   public:
    explicit IoUringTcpListener(IoUringExecutor& executor);
    ~IoUringTcpListener();

    // Non-copyable, non-movable
    IoUringTcpListener(const IoUringTcpListener&) = delete;
    IoUringTcpListener& operator=(const IoUringTcpListener&) = delete;

    // Synchronous setup
    [[nodiscard]] int Bind(const std::string& host, uint16_t port);
    [[nodiscard]] int Listen(int backlog = 128);
    [[nodiscard]] uint16_t GetPort() const;

    // Async accept — returns Task yielding unique_ptr<IoUringTcpStream>
    [[nodiscard]] Task<std::unique_ptr<IoUringTcpStream>> Accept();

    IoUringExecutor& GetExecutor() { return executor_; }

   private:
    IoUringExecutor& executor_;
    int listen_fd_ = -1;
    bool closed_ = false;
};

// ============================================================================
// IoUringTcpStream - Async TCP Connection via io_uring
// ============================================================================
class IoUringTcpStream {
   public:
    explicit IoUringTcpStream(IoUringExecutor& executor);
    ~IoUringTcpStream();

    // Non-copyable, non-movable
    IoUringTcpStream(const IoUringTcpStream&) = delete;
    IoUringTcpStream& operator=(const IoUringTcpStream&) = delete;

    // Connect to a remote host
    [[nodiscard]] Task<int> Connect(const std::string& host, uint16_t port);

    // Read up to max_bytes. Returns data on success, empty vector on EOF.
    // Sets closed_ only on EOF (result==0), not on errors.
    [[nodiscard]] Task<std::vector<char>> Read(size_t max_bytes = 4096);

    // Write data, returns bytes written or negative error.
    // On partial write followed by error, returns bytes successfully sent.
    [[nodiscard]] Task<ssize_t> Write(std::string_view data);

    void Close();
    [[nodiscard]] bool IsOpen() const { return fd_ >= 0 && !closed_; }
    void InitFromFd(int fd);

   private:
    IoUringExecutor& executor_;
    int fd_ = -1;
    bool closed_ = false;
};

}  // namespace hotcoco

#endif  // HOTCOCO_HAS_IOURING
