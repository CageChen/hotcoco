// ============================================================================
// hotcoco/io/sync_tcp.hpp - Synchronous TCP Networking
// ============================================================================
//
// Simple synchronous TCP for cases where async is overkill.
// Uses blocking sockets directly - no libuv/executor required.
//
// USAGE:
// ------
//   // Server
//   auto listener = SyncTcpListener::Listen("0.0.0.0", 8080);
//   auto conn = listener->Accept();
//   auto data = conn->RecvExact(100);
//   conn->Send("Hello");
//
//   // Client
//   auto conn = SyncTcpStream::Connect("127.0.0.1", 8080);
//   conn->Send("Hello");
//   auto data = conn->RecvExact(100);
//
// ============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "hotcoco/core/error.hpp"
#include "hotcoco/core/result.hpp"

namespace hotcoco {

// ============================================================================
// SyncTcpStream - Blocking TCP Connection
// ============================================================================
class SyncTcpStream {
public:
    ~SyncTcpStream();

    // Non-copyable, movable
    SyncTcpStream(const SyncTcpStream&) = delete;
    SyncTcpStream& operator=(const SyncTcpStream&) = delete;
    SyncTcpStream(SyncTcpStream&& other) noexcept;
    SyncTcpStream& operator=(SyncTcpStream&& other) noexcept;

    // ========================================================================
    // Factory
    // ========================================================================

    // Connect to a server (blocking)
    static Result<std::unique_ptr<SyncTcpStream>, std::error_code> Connect(
        const std::string& host, uint16_t port);

    // ========================================================================
    // Data Transfer
    // ========================================================================

    // Send data (blocking, returns bytes sent)
    Result<ssize_t, std::error_code> Send(std::string_view data);

    // Send all data (blocking)
    Result<void, std::error_code> SendAll(std::string_view data);

    // Receive up to max_bytes (blocking)
    Result<std::vector<char>, std::error_code> Recv(size_t max_bytes = 4096);

    // Receive exactly n bytes (blocking)
    Result<std::string, std::error_code> RecvExact(size_t n);

    // ========================================================================
    // Connection Management
    // ========================================================================

    void Close();
    bool IsOpen() const { return fd_ >= 0; }
    int GetFd() const { return fd_; }

private:
    friend class SyncTcpListener;

    SyncTcpStream() = default;
    explicit SyncTcpStream(int fd) : fd_(fd) {}

    int fd_ = -1;
};

// ============================================================================
// SyncTcpListener - Blocking TCP Server
// ============================================================================
class SyncTcpListener {
public:
    ~SyncTcpListener();

    // Non-copyable, movable
    SyncTcpListener(const SyncTcpListener&) = delete;
    SyncTcpListener& operator=(const SyncTcpListener&) = delete;
    SyncTcpListener(SyncTcpListener&& other) noexcept;
    SyncTcpListener& operator=(SyncTcpListener&& other) noexcept;

    // ========================================================================
    // Factory
    // ========================================================================

    // Create a listening socket
    static Result<std::unique_ptr<SyncTcpListener>, std::error_code> Listen(
        const std::string& host, uint16_t port, int backlog = 128);

    // ========================================================================
    // Accept
    // ========================================================================

    // Accept a connection (blocking)
    Result<std::unique_ptr<SyncTcpStream>, std::error_code> Accept();

    // ========================================================================
    // Properties
    // ========================================================================

    uint16_t GetPort() const { return port_; }
    int GetFd() const { return fd_; }

private:
    SyncTcpListener() = default;

    int fd_ = -1;
    uint16_t port_ = 0;
};

}  // namespace hotcoco
