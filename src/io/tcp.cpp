// ============================================================================
// hotcoco/io/tcp.cpp - Async TCP Networking Implementation
// ============================================================================

#include "hotcoco/io/tcp.hpp"

#include <arpa/inet.h>
#include <netdb.h>

#include <cassert>
#include <cstring>

#include "hotcoco/io/libuv_executor.hpp"

namespace hotcoco {

// ============================================================================
// TcpListener Implementation
// ============================================================================

TcpListener::TcpListener(LibuvExecutor& executor) : executor_(executor) {
    server_ = new uv_tcp_t;
    uv_tcp_init(GetLoop(), server_);
    server_->data = this;
}

TcpListener::~TcpListener() {
    if (!closed_) {
        closed_ = true;
        // server_ is heap-allocated, so uv_close's callback can safely
        // free it even after this TcpListener object is destroyed.
        uv_close(reinterpret_cast<uv_handle_t*>(server_),
                 [](uv_handle_t* h) {
                     delete reinterpret_cast<uv_tcp_t*>(h);
                 });
    }
}

uv_loop_t* TcpListener::GetLoop() {
    return executor_.GetLoop();
}

int TcpListener::Bind(const std::string& host, uint16_t port) {
    struct sockaddr_in addr;
    int result = uv_ip4_addr(host.c_str(), port, &addr);
    if (result != 0) {
        return result;
    }

    return uv_tcp_bind(server_, reinterpret_cast<const sockaddr*>(&addr), 0);
}

int TcpListener::Listen(int backlog) {
    return uv_listen(reinterpret_cast<uv_stream_t*>(server_), backlog, OnConnection);
}

void TcpListener::OnConnection(uv_stream_t* server, int status) {
    auto* self = static_cast<TcpListener*>(server->data);

    if (status < 0) {
        // Connection error - could log or handle
        return;
    }

    // Create a new TcpStream for this connection
    auto stream = std::make_unique<TcpStream>(self->executor_);
    stream->InitFromAccept(server);

    // If someone is waiting for accept, resume them
    if (self->accept_waiter_) {
        self->pending_connections_.push(std::move(stream));
        auto waiter = self->accept_waiter_;
        self->accept_waiter_ = nullptr;
        waiter.resume();
    } else {
        // Queue the connection for later Accept() call
        self->pending_connections_.push(std::move(stream));
    }
}

// AcceptAwaitable implementation
bool TcpListener::AcceptAwaitable::await_ready() const noexcept {
    return !listener_.pending_connections_.empty();
}

void TcpListener::AcceptAwaitable::await_suspend(std::coroutine_handle<> handle) {
    listener_.accept_waiter_ = handle;
}

std::unique_ptr<TcpStream> TcpListener::AcceptAwaitable::await_resume() {
    if (listener_.pending_connections_.empty()) {
        return nullptr;
    }
    auto stream = std::move(listener_.pending_connections_.front());
    listener_.pending_connections_.pop();
    return stream;
}

// ============================================================================
// TcpStream Implementation
// ============================================================================

TcpStream::TcpStream(LibuvExecutor& executor) : executor_(executor) {
    socket_ = new uv_tcp_t;
    std::memset(socket_, 0, sizeof(*socket_));
    std::memset(&connect_req_, 0, sizeof(connect_req_));
}

TcpStream::~TcpStream() {
    if (!closed_) {
        closed_ = true;

        if (reading_ && initialized_) {
            uv_read_stop(reinterpret_cast<uv_stream_t*>(socket_));
            reading_ = false;
        }

        if (initialized_) {
            // socket_ is heap-allocated, so uv_close's callback can safely
            // free it even after this TcpStream object is destroyed.
            uv_close(reinterpret_cast<uv_handle_t*>(socket_),
                     [](uv_handle_t* h) {
                         delete reinterpret_cast<uv_tcp_t*>(h);
                     });
        } else {
            // Never initialized via uv_tcp_init — just free the memory.
            delete socket_;
        }
    }
}

void TcpStream::InitFromAccept(uv_stream_t* server) {
    uv_tcp_init(server->loop, socket_);
    socket_->data = this;
    initialized_ = true;

    int result = uv_accept(server, reinterpret_cast<uv_stream_t*>(socket_));
    if (result != 0) {
        // uv_tcp_init succeeded, so we must uv_close before marking closed.
        // The close callback will free the heap-allocated handle.
        uv_close(reinterpret_cast<uv_handle_t*>(socket_), OnClose);
        socket_ = nullptr;
        closed_ = true;
        initialized_ = false;
    }
}

void TcpStream::Close() {
    if (closed_) return;
    closed_ = true;

    if (reading_) {
        uv_read_stop(reinterpret_cast<uv_stream_t*>(socket_));
        reading_ = false;
    }

    uv_close(reinterpret_cast<uv_handle_t*>(socket_), OnClose);
}

void TcpStream::OnClose(uv_handle_t* handle) {
    // Free the heap-allocated handle
    delete reinterpret_cast<uv_tcp_t*>(handle);
}

// ============================================================================
// Connect Implementation
// ============================================================================

bool TcpStream::ConnectAwaitable::await_suspend(std::coroutine_handle<> handle) {
    stream_.connect_waiter_ = handle;

    // Initialize the socket
    // We need access to the loop - get it from executor
    uv_loop_t* loop = stream_.executor_.GetLoop();
    uv_tcp_init(loop, stream_.socket_);
    stream_.socket_->data = &stream_;
    stream_.initialized_ = true;

    // Resolve address
    struct sockaddr_in addr;
    int result = uv_ip4_addr(host_.c_str(), port_, &addr);
    if (result != 0) {
        sync_error_ = result;
        return false;  // Don't suspend, resume immediately
    }

    stream_.connect_req_.data = &stream_;
    result = uv_tcp_connect(&stream_.connect_req_,
                            stream_.socket_,
                            reinterpret_cast<const sockaddr*>(&addr),
                            OnConnect);
    if (result != 0) {
        sync_error_ = result;
        return false;  // Don't suspend, resume immediately
    }
    return true;
}

void TcpStream::OnConnect(uv_connect_t* req, int status) {
    auto* self = static_cast<TcpStream*>(req->data);
    self->connect_result_ = status;

    if (self->connect_waiter_) {
        auto waiter = self->connect_waiter_;
        self->connect_waiter_ = nullptr;
        waiter.resume();
    }
}

// ============================================================================
// Read Implementation
// ============================================================================

bool TcpStream::ReadAwaitable::await_ready() const noexcept {
    return !stream_.read_buffer_.empty();
}

void TcpStream::ReadAwaitable::await_suspend(std::coroutine_handle<> handle) {
    stream_.read_waiter_ = handle;
    stream_.reading_ = true;

    uv_read_start(reinterpret_cast<uv_stream_t*>(stream_.socket_),
                  AllocBuffer,
                  OnRead);
}

std::vector<char> TcpStream::ReadAwaitable::await_resume() {
    stream_.reading_ = false;
    uv_read_stop(reinterpret_cast<uv_stream_t*>(stream_.socket_));

    std::vector<char> result;
    std::swap(result, stream_.read_buffer_);
    return result;
}

void TcpStream::AllocBuffer(uv_handle_t* handle, size_t suggested, uv_buf_t* buf) {
    auto* self = static_cast<TcpStream*>(handle->data);
    // Append to existing data rather than overwriting. If libuv calls
    // AllocBuffer + OnRead multiple times before the coroutine resumes,
    // earlier data is preserved.
    size_t size = suggested > 0 ? suggested : 4096;
    size_t current_size = self->read_buffer_.size();
    self->read_buffer_.resize(current_size + size);
    buf->base = self->read_buffer_.data() + current_size;
    buf->len = size;
}

void TcpStream::OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* self = static_cast<TcpStream*>(stream->data);

    if (nread < 0) {
        // Error or EOF — remove the unused allocation space
        if (buf && buf->len > 0) {
            self->read_buffer_.resize(self->read_buffer_.size() - buf->len);
        }
        self->closed_ = true;
    } else if (nread > 0) {
        // Trim unused allocation: we allocated buf->len but only got nread bytes
        size_t unused = buf->len - static_cast<size_t>(nread);
        self->read_buffer_.resize(self->read_buffer_.size() - unused);
    } else {
        // nread == 0, EAGAIN — remove the unused allocation
        if (buf && buf->len > 0) {
            self->read_buffer_.resize(self->read_buffer_.size() - buf->len);
        }
    }

    self->read_result_ = nread;

    // Resume the waiting coroutine
    if (self->read_waiter_) {
        auto waiter = self->read_waiter_;
        self->read_waiter_ = nullptr;
        waiter.resume();
    }
}

// ============================================================================
// Write Implementation
// ============================================================================

struct WriteRequest {
    uv_write_t req;
    TcpStream* stream;
    std::coroutine_handle<> waiter;
    int* result;
    std::string data;  // Keep data alive during write
};

bool TcpStream::WriteAwaitable::await_suspend(std::coroutine_handle<> handle) {
    // Create write request that owns the data
    auto* write_req = new WriteRequest;
    write_req->stream = &stream_;
    write_req->waiter = handle;
    write_req->result = &bytes_written_;
    write_req->data = std::string(data_);
    write_req->req.data = write_req;

    uv_buf_t buf = uv_buf_init(write_req->data.data(),
                               static_cast<unsigned int>(write_req->data.size()));

    int result = uv_write(&write_req->req,
                          reinterpret_cast<uv_stream_t*>(stream_.socket_),
                          &buf, 1,
                          OnWrite);

    if (result != 0) {
        bytes_written_ = result;
        delete write_req;
        return false;  // Don't suspend, resume immediately
    }
    return true;
}

void TcpStream::OnWrite(uv_write_t* req, int status) {
    auto* write_req = static_cast<WriteRequest*>(req->data);

    if (status >= 0) {
        *write_req->result = static_cast<int>(write_req->data.size());
    } else {
        *write_req->result = status;
    }

    auto waiter = write_req->waiter;
    delete write_req;
    waiter.resume();
}

}  // namespace hotcoco
