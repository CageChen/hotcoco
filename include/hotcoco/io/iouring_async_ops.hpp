// ============================================================================
// hotcoco/io/iouring_async_ops.hpp - Low-level io_uring Async Operations
// ============================================================================
//
// Provides 1:1 mappings to io_uring_prep_* as coroutine awaitables.
// Each function returns an awaitable; co_await yields int32_t (cqe->res):
//   - Positive: success / byte count / fd
//   - Zero: EOF (for read/recv)
//   - Negative: -errno
//
// These are building blocks for higher-level abstractions like
// IoUringTcpListener, IoUringTcpStream, and File.
//
// NETWORKING:
// -----------
//   int fd = co_await IoUringAccept(listen_fd);
//   int n  = co_await IoUringRecv(fd, buf, len, 0);
//   int n  = co_await IoUringSend(fd, buf, len, MSG_NOSIGNAL);
//
// FILE I/O:
// ---------
//   int fd = co_await IoUringOpen("/tmp/data.bin", O_RDWR | O_CREAT);
//   int n  = co_await IoUringReadFile(fd, buf, len, offset);
//   int n  = co_await IoUringWriteFile(fd, buf, len, offset);
//   int r  = co_await IoUringFsync(fd);
//   int r  = co_await IoUringClose(fd);
//
// ============================================================================

#pragma once

#ifdef HOTCOCO_HAS_IOURING

#include "hotcoco/io/executor.hpp"
#include "hotcoco/io/iouring_executor.hpp"

#include <sys/socket.h>
#include <sys/stat.h>

#include <coroutine>
#include <cstdint>
#include <fcntl.h>
#include <liburing.h>
#include <string>

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
    IoUringExecutor::OpContext ctx_{IoUringExecutor::OpType::IO, nullptr, 0, 0, {}};
};

[[nodiscard]] inline IoUringAcceptAwaitable IoUringAccept(int listen_fd, struct sockaddr* addr = nullptr,
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
    IoUringExecutor::OpContext ctx_{IoUringExecutor::OpType::IO, nullptr, 0, 0, {}};
};

[[nodiscard]] inline IoUringConnectAwaitable IoUringConnect(int fd, const struct sockaddr* addr, socklen_t addrlen) {
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

[[nodiscard]] inline IoUringRecvAwaitable IoUringRecv(int fd, void* buf, size_t len, int flags = 0) {
    return IoUringRecvAwaitable(fd, buf, len, flags);
}

// ============================================================================
// IoUringRecvProvidedAwaitable (Single-shot Recv with Provided Buffers)
// ============================================================================
class IoUringRecvProvidedAwaitable {
   public:
    IoUringRecvProvidedAwaitable(int fd, uint16_t bgid, int flags) : fd_(fd), bgid_(bgid), flags_(flags) {}

    // Non-copyable, non-movable
    IoUringRecvProvidedAwaitable(const IoUringRecvProvidedAwaitable&) = delete;
    IoUringRecvProvidedAwaitable& operator=(const IoUringRecvProvidedAwaitable&) = delete;
    IoUringRecvProvidedAwaitable(IoUringRecvProvidedAwaitable&&) = delete;
    IoUringRecvProvidedAwaitable& operator=(IoUringRecvProvidedAwaitable&&) = delete;

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

        // Use single-shot receive but tell the kernel to pick a buffer from the ring
        io_uring_prep_recv(sqe, fd_, nullptr, 0, flags_);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = bgid_;

        io_uring_sqe_set_data(sqe, &ctx_);
        // Do not immediately `io_uring_submit()`. Batching is handled by the executor.
        return true;
    }

    // Returns a struct containing both the byte count and the buffer ID chosen by the kernel.
    struct Result {
        int32_t res;
        uint32_t flags;
    };

    Result await_resume() const noexcept { return {ctx_.result, ctx_.cqe_flags}; }

   private:
    int fd_;
    uint16_t bgid_;
    int flags_;
    IoUringExecutor::OpContext ctx_{IoUringExecutor::OpType::IO, nullptr, 0, 0, {}};
};

[[nodiscard]] inline IoUringRecvProvidedAwaitable IoUringRecvProvided(int fd, uint16_t bgid, int flags = 0) {
    return IoUringRecvProvidedAwaitable(fd, bgid, flags);
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
    IoUringExecutor::OpContext ctx_{IoUringExecutor::OpType::IO, nullptr, 0, 0, {}};
};

[[nodiscard]] inline IoUringSendAwaitable IoUringSend(int fd, const void* buf, size_t len, int flags = MSG_NOSIGNAL) {
    return IoUringSendAwaitable(fd, buf, len, flags);
}

// ============================================================================
// File I/O Awaitables
// ============================================================================

// ============================================================================
// IoUringOpenAtAwaitable - Async file open via IORING_OP_OPENAT
// ============================================================================
class IoUringOpenAtAwaitable {
   public:
    IoUringOpenAtAwaitable(int dirfd, std::string path, int flags, mode_t mode)
        : dirfd_(dirfd), path_(std::move(path)), flags_(flags), mode_(mode) {}

    // Non-copyable, non-movable (OpContext address submitted to kernel)
    IoUringOpenAtAwaitable(const IoUringOpenAtAwaitable&) = delete;
    IoUringOpenAtAwaitable& operator=(const IoUringOpenAtAwaitable&) = delete;
    IoUringOpenAtAwaitable(IoUringOpenAtAwaitable&&) = delete;
    IoUringOpenAtAwaitable& operator=(IoUringOpenAtAwaitable&&) = delete;

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

        io_uring_prep_openat(sqe, dirfd_, path_.c_str(), flags_, mode_);
        io_uring_sqe_set_data(sqe, &ctx_);
        io_uring_submit(executor->GetRing());
        return true;
    }

    int32_t await_resume() const noexcept { return ctx_.result; }

   private:
    int dirfd_;
    std::string path_;  // Owned copy — pointer passed to kernel must stay valid
    int flags_;
    mode_t mode_;
    IoUringExecutor::OpContext ctx_{IoUringExecutor::OpType::IO, nullptr, 0, 0, {}};
};

[[nodiscard]] inline IoUringOpenAtAwaitable IoUringOpenAt(int dirfd, std::string path, int flags, mode_t mode = 0644) {
    return IoUringOpenAtAwaitable(dirfd, std::move(path), flags, mode);
}

// Convenience: open relative to cwd
[[nodiscard]] inline IoUringOpenAtAwaitable IoUringOpen(std::string path, int flags, mode_t mode = 0644) {
    return IoUringOpenAt(AT_FDCWD, std::move(path), flags, mode);
}

// ============================================================================
// IoUringReadFileAwaitable - Async positional read via IORING_OP_READ
// ============================================================================
// NOTE: Named IoUringReadFile to avoid collision with IoUringRecv (socket recv).
class IoUringReadFileAwaitable {
   public:
    IoUringReadFileAwaitable(int fd, void* buf, unsigned nbytes, uint64_t offset)
        : fd_(fd), buf_(buf), nbytes_(nbytes), offset_(offset) {}

    // Non-copyable, non-movable (OpContext address submitted to kernel)
    IoUringReadFileAwaitable(const IoUringReadFileAwaitable&) = delete;
    IoUringReadFileAwaitable& operator=(const IoUringReadFileAwaitable&) = delete;
    IoUringReadFileAwaitable(IoUringReadFileAwaitable&&) = delete;
    IoUringReadFileAwaitable& operator=(IoUringReadFileAwaitable&&) = delete;

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

        io_uring_prep_read(sqe, fd_, buf_, nbytes_, offset_);
        io_uring_sqe_set_data(sqe, &ctx_);
        io_uring_submit(executor->GetRing());
        return true;
    }

    int32_t await_resume() const noexcept { return ctx_.result; }

   private:
    int fd_;
    void* buf_;
    unsigned nbytes_;
    uint64_t offset_;
    IoUringExecutor::OpContext ctx_{IoUringExecutor::OpType::IO, nullptr, 0, 0, {}};
};

[[nodiscard]] inline IoUringReadFileAwaitable IoUringReadFile(int fd, void* buf, unsigned nbytes, uint64_t offset = 0) {
    return IoUringReadFileAwaitable(fd, buf, nbytes, offset);
}

// ============================================================================
// IoUringWriteFileAwaitable - Async positional write via IORING_OP_WRITE
// ============================================================================
// NOTE: Named IoUringWriteFile to avoid collision with IoUringSend (socket send).
class IoUringWriteFileAwaitable {
   public:
    IoUringWriteFileAwaitable(int fd, const void* buf, unsigned nbytes, uint64_t offset)
        : fd_(fd), buf_(buf), nbytes_(nbytes), offset_(offset) {}

    // Non-copyable, non-movable (OpContext address submitted to kernel)
    IoUringWriteFileAwaitable(const IoUringWriteFileAwaitable&) = delete;
    IoUringWriteFileAwaitable& operator=(const IoUringWriteFileAwaitable&) = delete;
    IoUringWriteFileAwaitable(IoUringWriteFileAwaitable&&) = delete;
    IoUringWriteFileAwaitable& operator=(IoUringWriteFileAwaitable&&) = delete;

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

        io_uring_prep_write(sqe, fd_, buf_, nbytes_, offset_);
        io_uring_sqe_set_data(sqe, &ctx_);
        io_uring_submit(executor->GetRing());
        return true;
    }

    int32_t await_resume() const noexcept { return ctx_.result; }

   private:
    int fd_;
    const void* buf_;
    unsigned nbytes_;
    uint64_t offset_;
    IoUringExecutor::OpContext ctx_{IoUringExecutor::OpType::IO, nullptr, 0, 0, {}};
};

[[nodiscard]] inline IoUringWriteFileAwaitable IoUringWriteFile(int fd, const void* buf, unsigned nbytes,
                                                                uint64_t offset = 0) {
    return IoUringWriteFileAwaitable(fd, buf, nbytes, offset);
}

// ============================================================================
// IoUringCloseAwaitable - Async file close via IORING_OP_CLOSE
// ============================================================================
class IoUringCloseAwaitable {
   public:
    explicit IoUringCloseAwaitable(int fd) : fd_(fd) {}

    // Non-copyable, non-movable (OpContext address submitted to kernel)
    IoUringCloseAwaitable(const IoUringCloseAwaitable&) = delete;
    IoUringCloseAwaitable& operator=(const IoUringCloseAwaitable&) = delete;
    IoUringCloseAwaitable(IoUringCloseAwaitable&&) = delete;
    IoUringCloseAwaitable& operator=(IoUringCloseAwaitable&&) = delete;

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

        io_uring_prep_close(sqe, fd_);
        io_uring_sqe_set_data(sqe, &ctx_);
        io_uring_submit(executor->GetRing());
        return true;
    }

    int32_t await_resume() const noexcept { return ctx_.result; }

   private:
    int fd_;
    IoUringExecutor::OpContext ctx_{IoUringExecutor::OpType::IO, nullptr, 0, 0, {}};
};

[[nodiscard]] inline IoUringCloseAwaitable IoUringClose(int fd) {
    return IoUringCloseAwaitable(fd);
}

// ============================================================================
// IoUringFsyncAwaitable - Async fsync via IORING_OP_FSYNC
// ============================================================================
class IoUringFsyncAwaitable {
   public:
    IoUringFsyncAwaitable(int fd, unsigned flags) : fd_(fd), flags_(flags) {}

    // Non-copyable, non-movable (OpContext address submitted to kernel)
    IoUringFsyncAwaitable(const IoUringFsyncAwaitable&) = delete;
    IoUringFsyncAwaitable& operator=(const IoUringFsyncAwaitable&) = delete;
    IoUringFsyncAwaitable(IoUringFsyncAwaitable&&) = delete;
    IoUringFsyncAwaitable& operator=(IoUringFsyncAwaitable&&) = delete;

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

        io_uring_prep_fsync(sqe, fd_, flags_);
        io_uring_sqe_set_data(sqe, &ctx_);
        io_uring_submit(executor->GetRing());
        return true;
    }

    int32_t await_resume() const noexcept { return ctx_.result; }

   private:
    int fd_;
    unsigned flags_;
    IoUringExecutor::OpContext ctx_{IoUringExecutor::OpType::IO, nullptr, 0, 0, {}};
};

[[nodiscard]] inline IoUringFsyncAwaitable IoUringFsync(int fd, unsigned flags = 0) {
    return IoUringFsyncAwaitable(fd, flags);
}

// ============================================================================
// IoUringStatxAwaitable - Async stat via IORING_OP_STATX
// ============================================================================
class IoUringStatxAwaitable {
   public:
    IoUringStatxAwaitable(int dirfd, std::string path, int flags, unsigned mask, struct statx* statxbuf)
        : dirfd_(dirfd), path_(std::move(path)), flags_(flags), mask_(mask), statxbuf_(statxbuf) {}

    // Non-copyable, non-movable (OpContext address submitted to kernel)
    IoUringStatxAwaitable(const IoUringStatxAwaitable&) = delete;
    IoUringStatxAwaitable& operator=(const IoUringStatxAwaitable&) = delete;
    IoUringStatxAwaitable(IoUringStatxAwaitable&&) = delete;
    IoUringStatxAwaitable& operator=(IoUringStatxAwaitable&&) = delete;

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

        io_uring_prep_statx(sqe, dirfd_, path_.c_str(), flags_, mask_, statxbuf_);
        io_uring_sqe_set_data(sqe, &ctx_);
        io_uring_submit(executor->GetRing());
        return true;
    }

    int32_t await_resume() const noexcept { return ctx_.result; }

   private:
    int dirfd_;
    std::string path_;  // Owned copy — pointer passed to kernel must stay valid
    int flags_;
    unsigned mask_;
    struct statx* statxbuf_;
    IoUringExecutor::OpContext ctx_{IoUringExecutor::OpType::IO, nullptr, 0, 0, {}};
};

[[nodiscard]] inline IoUringStatxAwaitable IoUringStatx(int dirfd, std::string path, int flags, unsigned mask,
                                                        struct statx* statxbuf) {
    return IoUringStatxAwaitable(dirfd, std::move(path), flags, mask, statxbuf);
}

// ============================================================================
// IoUringUnlinkAtAwaitable - Async unlink via IORING_OP_UNLINKAT
// ============================================================================
class IoUringUnlinkAtAwaitable {
   public:
    IoUringUnlinkAtAwaitable(int dirfd, std::string path, int flags)
        : dirfd_(dirfd), path_(std::move(path)), flags_(flags) {}

    // Non-copyable, non-movable (OpContext address submitted to kernel)
    IoUringUnlinkAtAwaitable(const IoUringUnlinkAtAwaitable&) = delete;
    IoUringUnlinkAtAwaitable& operator=(const IoUringUnlinkAtAwaitable&) = delete;
    IoUringUnlinkAtAwaitable(IoUringUnlinkAtAwaitable&&) = delete;
    IoUringUnlinkAtAwaitable& operator=(IoUringUnlinkAtAwaitable&&) = delete;

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

        io_uring_prep_unlinkat(sqe, dirfd_, path_.c_str(), flags_);
        io_uring_sqe_set_data(sqe, &ctx_);
        io_uring_submit(executor->GetRing());
        return true;
    }

    int32_t await_resume() const noexcept { return ctx_.result; }

   private:
    int dirfd_;
    std::string path_;  // Owned copy — pointer passed to kernel must stay valid
    int flags_;
    IoUringExecutor::OpContext ctx_{IoUringExecutor::OpType::IO, nullptr, 0, 0, {}};
};

[[nodiscard]] inline IoUringUnlinkAtAwaitable IoUringUnlinkAt(int dirfd, std::string path, int flags = 0) {
    return IoUringUnlinkAtAwaitable(dirfd, std::move(path), flags);
}

// ============================================================================
// IoUringMkdirAtAwaitable - Async mkdir via IORING_OP_MKDIRAT
// ============================================================================
class IoUringMkdirAtAwaitable {
   public:
    IoUringMkdirAtAwaitable(int dirfd, std::string path, mode_t mode)
        : dirfd_(dirfd), path_(std::move(path)), mode_(mode) {}

    // Non-copyable, non-movable (OpContext address submitted to kernel)
    IoUringMkdirAtAwaitable(const IoUringMkdirAtAwaitable&) = delete;
    IoUringMkdirAtAwaitable& operator=(const IoUringMkdirAtAwaitable&) = delete;
    IoUringMkdirAtAwaitable(IoUringMkdirAtAwaitable&&) = delete;
    IoUringMkdirAtAwaitable& operator=(IoUringMkdirAtAwaitable&&) = delete;

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

        io_uring_prep_mkdirat(sqe, dirfd_, path_.c_str(), mode_);
        io_uring_sqe_set_data(sqe, &ctx_);
        io_uring_submit(executor->GetRing());
        return true;
    }

    int32_t await_resume() const noexcept { return ctx_.result; }

   private:
    int dirfd_;
    std::string path_;  // Owned copy — pointer passed to kernel must stay valid
    mode_t mode_;
    IoUringExecutor::OpContext ctx_{IoUringExecutor::OpType::IO, nullptr, 0, 0, {}};
};

[[nodiscard]] inline IoUringMkdirAtAwaitable IoUringMkdirAt(int dirfd, std::string path, mode_t mode = 0755) {
    return IoUringMkdirAtAwaitable(dirfd, std::move(path), mode);
}

}  // namespace hotcoco

#endif  // HOTCOCO_HAS_IOURING
