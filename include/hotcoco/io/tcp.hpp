// ============================================================================
// hotcoco/io/tcp.hpp - Async TCP Networking
// ============================================================================
//
// This header provides async TCP networking primitives:
// - TcpListener: Accept incoming connections
// - TcpStream: Read/write data asynchronously
//
// These are built on libuv's TCP support and integrate with our Executor
// for seamless coroutine usage.
//
// USAGE:
// ------
//   Task<void> Server() {
//       TcpListener listener(executor);
//       co_await listener.Bind("0.0.0.0", 8080);
//       co_await listener.Listen();
//
//       while (true) {
//           auto stream = co_await listener.Accept();
//           // Handle connection...
//       }
//   }
//
// ============================================================================

#pragma once

#include <uv.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

#include "hotcoco/io/executor.hpp"

namespace hotcoco {

// Forward declarations
class TcpStream;
class LibuvExecutor;

// ============================================================================
// TcpListener - Accept Incoming Connections
// ============================================================================
//
// TcpListener binds to an address and listens for incoming TCP connections.
// Use Accept() to get new connections as TcpStream objects.
//
class TcpListener {
public:
    explicit TcpListener(LibuvExecutor& executor);
    ~TcpListener();

    // Non-copyable, non-movable (libuv handles can't be moved)
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // ========================================================================
    // Server Setup
    // ========================================================================

    // Bind to an address and port
    // Returns 0 on success, negative libuv error code on failure
    int Bind(const std::string& host, uint16_t port);

    // Start listening for connections
    // backlog: Maximum pending connections (default 128)
    // Returns 0 on success, negative libuv error code on failure
    int Listen(int backlog = 128);

    // ========================================================================
    // Accept Awaitable
    // ========================================================================
    // Returns an awaitable that yields new TcpStream connections

    class AcceptAwaitable {
    public:
        explicit AcceptAwaitable(TcpListener& listener) : listener_(listener) {}

        bool await_ready() const noexcept;
        void await_suspend(std::coroutine_handle<> handle);
        std::unique_ptr<TcpStream> await_resume();

    private:
        TcpListener& listener_;
    };

    AcceptAwaitable Accept() { return AcceptAwaitable(*this); }

    // ========================================================================
    // Internal
    // ========================================================================
    LibuvExecutor& GetExecutor() { return executor_; }
    uv_loop_t* GetLoop();

private:
    friend class AcceptAwaitable;

    static void OnConnection(uv_stream_t* server, int status);

    LibuvExecutor& executor_;
    uv_tcp_t* server_;  // Heap-allocated so uv_close callback can safely access it
    std::queue<std::unique_ptr<TcpStream>> pending_connections_;
    std::coroutine_handle<> accept_waiter_;
    bool closed_ = false;
    bool bound_ = false;
};

// ============================================================================
// TcpStream - Async TCP Connection
// ============================================================================
//
// TcpStream represents a TCP connection for reading and writing data.
// All operations are async and can be co_awaited.
//
class TcpStream {
public:
    explicit TcpStream(LibuvExecutor& executor);
    ~TcpStream();

    // Non-copyable, non-movable
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    // ========================================================================
    // Client Connection
    // ========================================================================

    class ConnectAwaitable {
    public:
        ConnectAwaitable(TcpStream& stream, const std::string& host, uint16_t port)
            : stream_(stream), host_(host), port_(port) {}

        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> handle);
        int await_resume() { return sync_error_ ? sync_error_ : stream_.connect_result_; }

    private:
        TcpStream& stream_;
        std::string host_;
        uint16_t port_;
        int sync_error_ = 0;  // Set on synchronous errors (await_suspend returns false)
    };

    ConnectAwaitable Connect(const std::string& host, uint16_t port) {
        return ConnectAwaitable(*this, host, port);
    }

    // ========================================================================
    // Reading
    // ========================================================================

    class ReadAwaitable {
    public:
        ReadAwaitable(TcpStream& stream, size_t max_bytes)
            : stream_(stream), max_bytes_(max_bytes) {}

        bool await_ready() const noexcept;
        void await_suspend(std::coroutine_handle<> handle);
        std::vector<char> await_resume();

    private:
        TcpStream& stream_;
        size_t max_bytes_;
    };

    ReadAwaitable Read(size_t max_bytes = 4096) {
        return ReadAwaitable(*this, max_bytes);
    }

    // ========================================================================
    // Writing
    // ========================================================================

    class WriteAwaitable {
    public:
        WriteAwaitable(TcpStream& stream, std::string_view data)
            : stream_(stream), data_(data) {}

        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> handle);
        int await_resume() { return bytes_written_; }

    private:
        TcpStream& stream_;
        std::string_view data_;
        int bytes_written_ = 0;
    };

    WriteAwaitable Write(std::string_view data) {
        return WriteAwaitable(*this, data);
    }

    // ========================================================================
    // Connection Management
    // ========================================================================

    // Close the connection
    void Close();

    // Check if connection is open
    bool IsOpen() const { return !closed_; }

    // Get the underlying handle (for TcpListener to initialize)
    uv_tcp_t* GetHandle() { return socket_; }

    // Initialize from accepted connection
    void InitFromAccept(uv_stream_t* server);

private:
    friend class ConnectAwaitable;
    friend class ReadAwaitable;
    friend class WriteAwaitable;

    static void OnConnect(uv_connect_t* req, int status);
    static void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
    static void OnWrite(uv_write_t* req, int status);
    static void AllocBuffer(uv_handle_t* handle, size_t suggested, uv_buf_t* buf);
    static void OnClose(uv_handle_t* handle);

    LibuvExecutor& executor_;
    uv_tcp_t* socket_;  // Heap-allocated so uv_close callback can safely access it
    bool closed_ = false;
    bool initialized_ = false;  // True after uv_tcp_init; guards uv_close in destructor
    bool reading_ = false;

    // Read state
    std::vector<char> read_buffer_;
    std::coroutine_handle<> read_waiter_;
    ssize_t read_result_ = 0;

    // Connect state
    uv_connect_t connect_req_;
    std::coroutine_handle<> connect_waiter_;
    int connect_result_ = 0;
};

}  // namespace hotcoco
