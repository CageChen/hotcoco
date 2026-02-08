// ============================================================================
// hotcoco/io/iouring_async_ops.hpp - Low-level io_uring Async Operations
// ============================================================================
//
// Provides 1:1 mappings to io_uring_prep_* as coroutine awaitables.
// Each function returns an awaitable; co_await yields int32_t (cqe->res):
//   - Positive: success / byte count
//   - Zero: EOF (for recv)
//   - Negative: -errno
//
// These are building blocks for higher-level abstractions like
// IoUringTcpListener and IoUringTcpStream.
//
// USAGE:
// ------
//   int fd = co_await IoUringAccept(listen_fd, nullptr, nullptr);
//   if (fd < 0) { /* handle -errno */ }
//
//   int n = co_await IoUringRecv(fd, buf, len, 0);
//   int n = co_await IoUringSend(fd, buf, len, MSG_NOSIGNAL);
//
// ============================================================================

#pragma once

#ifdef HOTCOCO_HAS_IOURING

#include "hotcoco/io/executor.hpp"
#include "hotcoco/io/iouring_executor.hpp"

#include <sys/socket.h>

#include <coroutine>
#include <cstdint>
#include <liburing.h>

namespace hotcoco {

// ============================================================================
// IoUringAcceptAwaitable
// ============================================================================
class IoUringAcceptAwaitable {
   public:
    IoUringAcceptAwaitable(int listen_fd, struct sockaddr* addr, socklen_t* addrlen, int flags)
        : listen_fd_(listen_fd), addr_(addr), addrlen_(addrlen), flags_(flags) {}

    // Non-copyable, non-movable (OpContext address submitted to kernel)
    IoUringAcceptAwaitable(const IoUringAcceptAwaitable&) = delete;
    IoUringAcceptAwaitable& operator=(const IoUringAcceptAwaitable&) = delete;
    IoUringAcceptAwaitable(IoUringAcceptAwaitable&&) = delete;
    IoUringAcceptAwaitable& operator=(IoUringAcceptAwaitable&&) = delete;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
        auto* executor = dynamic_cast<IoUringExecutor*>(GetCurrentExecutor());
        if (!executor) {
            ctx_.result = -ENXIO;
            return false;
        }

        ctx_.handle = handle;
        auto* sqe = io_uring_get_sqe(executor->GetRing());
        if (!sqe) {
            ctx_.result = -EAGAIN;
            return false;
        }

        io_uring_prep_accept(sqe, listen_fd_, addr_, addrlen_, flags_ | SOCK_CLOEXEC | SOCK_NONBLOCK);
        io_uring_sqe_set_data(sqe, &ctx_);
        io_uring_submit(executor->GetRing());
        return true;
    }

    int32_t await_resume() const noexcept { return ctx_.result; }

   private:
    int listen_fd_;
    struct sockaddr* addr_;
    socklen_t* addrlen_;
    int flags_;
    IoUringExecutor::OpContext ctx_{IoUringExecutor::OpType::IO, nullptr, 0, {}};
};

inline IoUringAcceptAwaitable IoUringAccept(int listen_fd, struct sockaddr* addr = nullptr,
                                            socklen_t* addrlen = nullptr, int flags = 0) {
    return IoUringAcceptAwaitable(listen_fd, addr, addrlen, flags);
}

// ============================================================================
// IoUringConnectAwaitable
// ============================================================================
class IoUringConnectAwaitable {
   public:
    IoUringConnectAwaitable(int fd, const struct sockaddr* addr, socklen_t addrlen)
        : fd_(fd), addr_(addr), addrlen_(addrlen) {}

    // Non-copyable, non-movable (OpContext address submitted to kernel)
    IoUringConnectAwaitable(const IoUringConnectAwaitable&) = delete;
    IoUringConnectAwaitable& operator=(const IoUringConnectAwaitable&) = delete;
    IoUringConnectAwaitable(IoUringConnectAwaitable&&) = delete;
    IoUringConnectAwaitable& operator=(IoUringConnectAwaitable&&) = delete;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
        auto* executor = dynamic_cast<IoUringExecutor*>(GetCurrentExecutor());
        if (!executor) {
            ctx_.result = -ENXIO;
            return false;
        }

        ctx_.handle = handle;
        auto* sqe = io_uring_get_sqe(executor->GetRing());
        if (!sqe) {
            ctx_.result = -EAGAIN;
            return false;
        }

        io_uring_prep_connect(sqe, fd_, addr_, addrlen_);
        io_uring_sqe_set_data(sqe, &ctx_);
        io_uring_submit(executor->GetRing());
        return true;
    }

    int32_t await_resume() const noexcept { return ctx_.result; }

   private:
    int fd_;
    const struct sockaddr* addr_;
    socklen_t addrlen_;
    IoUringExecutor::OpContext ctx_{IoUringExecutor::OpType::IO, nullptr, 0, {}};
};

inline IoUringConnectAwaitable IoUringConnect(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    return IoUringConnectAwaitable(fd, addr, addrlen);
}

// ============================================================================
// IoUringRecvAwaitable
// ============================================================================
class IoUringRecvAwaitable {
   public:
    IoUringRecvAwaitable(int fd, void* buf, size_t len, int flags) : fd_(fd), buf_(buf), len_(len), flags_(flags) {}

    // Non-copyable, non-movable (OpContext address submitted to kernel)
    IoUringRecvAwaitable(const IoUringRecvAwaitable&) = delete;
    IoUringRecvAwaitable& operator=(const IoUringRecvAwaitable&) = delete;
    IoUringRecvAwaitable(IoUringRecvAwaitable&&) = delete;
    IoUringRecvAwaitable& operator=(IoUringRecvAwaitable&&) = delete;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
        auto* executor = dynamic_cast<IoUringExecutor*>(GetCurrentExecutor());
        if (!executor) {
            ctx_.result = -ENXIO;
            return false;
        }

        ctx_.handle = handle;
        auto* sqe = io_uring_get_sqe(executor->GetRing());
        if (!sqe) {
            ctx_.result = -EAGAIN;
            return false;
        }

        io_uring_prep_recv(sqe, fd_, buf_, static_cast<unsigned>(len_), flags_);
        io_uring_sqe_set_data(sqe, &ctx_);
        io_uring_submit(executor->GetRing());
        return true;
    }

    int32_t await_resume() const noexcept { return ctx_.result; }

   private:
    int fd_;
    void* buf_;
    size_t len_;
    int flags_;
    IoUringExecutor::OpContext ctx_{IoUringExecutor::OpType::IO, nullptr, 0, {}};
};

inline IoUringRecvAwaitable IoUringRecv(int fd, void* buf, size_t len, int flags = 0) {
    return IoUringRecvAwaitable(fd, buf, len, flags);
}

// ============================================================================
// IoUringSendAwaitable
// ============================================================================
class IoUringSendAwaitable {
   public:
    IoUringSendAwaitable(int fd, const void* buf, size_t len, int flags)
        : fd_(fd), buf_(buf), len_(len), flags_(flags) {}

    // Non-copyable, non-movable (OpContext address submitted to kernel)
    IoUringSendAwaitable(const IoUringSendAwaitable&) = delete;
    IoUringSendAwaitable& operator=(const IoUringSendAwaitable&) = delete;
    IoUringSendAwaitable(IoUringSendAwaitable&&) = delete;
    IoUringSendAwaitable& operator=(IoUringSendAwaitable&&) = delete;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
        auto* executor = dynamic_cast<IoUringExecutor*>(GetCurrentExecutor());
        if (!executor) {
            ctx_.result = -ENXIO;
            return false;
        }

        ctx_.handle = handle;
        auto* sqe = io_uring_get_sqe(executor->GetRing());
        if (!sqe) {
            ctx_.result = -EAGAIN;
            return false;
        }

        io_uring_prep_send(sqe, fd_, buf_, static_cast<unsigned>(len_), flags_);
        io_uring_sqe_set_data(sqe, &ctx_);
        io_uring_submit(executor->GetRing());
        return true;
    }

    int32_t await_resume() const noexcept { return ctx_.result; }

   private:
    int fd_;
    const void* buf_;
    size_t len_;
    int flags_;
    IoUringExecutor::OpContext ctx_{IoUringExecutor::OpType::IO, nullptr, 0, {}};
};

inline IoUringSendAwaitable IoUringSend(int fd, const void* buf, size_t len, int flags = MSG_NOSIGNAL) {
    return IoUringSendAwaitable(fd, buf, len, flags);
}

}  // namespace hotcoco

#endif  // HOTCOCO_HAS_IOURING
