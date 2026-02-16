// ============================================================================
// hotcoco/io/iouring_tcp.cpp - io_uring TCP Networking Implementation
// ============================================================================

#ifdef HOTCOCO_HAS_IOURING

#include "hotcoco/io/iouring_tcp.hpp"

#include "hotcoco/io/iouring_async_ops.hpp"

#include <sys/socket.h>

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <unistd.h>

namespace hotcoco {

// ============================================================================
// IoUringTcpListener Implementation
// ============================================================================

IoUringTcpListener::IoUringTcpListener(IoUringExecutor& executor) : executor_(executor) {}

IoUringTcpListener::~IoUringTcpListener() {
    if (!closed_ && listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        closed_ = true;
    }
}

int IoUringTcpListener::Bind(const std::string& host, uint16_t port) {
    if (listen_fd_ >= 0) {
        return -EALREADY;
    }

    // Create the socket
    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listen_fd_ < 0) {
        return -errno;
    }

    // Allow address reuse
    int optval = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    // Parse and bind
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close(listen_fd_);
        listen_fd_ = -1;
        return -EINVAL;
    }

    int ret = bind(listen_fd_, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
    if (ret < 0) {
        int err = errno;
        close(listen_fd_);
        listen_fd_ = -1;
        return -err;
    }

    return 0;
}

int IoUringTcpListener::Listen(int backlog) {
    if (listen_fd_ < 0) {
        return -EBADF;
    }
    int ret = listen(listen_fd_, backlog);
    if (ret < 0) {
        return -errno;
    }
    return 0;
}

uint16_t IoUringTcpListener::GetPort() const {
    if (listen_fd_ < 0) return 0;

    struct sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

Task<std::unique_ptr<IoUringTcpStream>> IoUringTcpListener::Accept() {
    int32_t fd = co_await IoUringAccept(listen_fd_, nullptr, nullptr, 0);
    if (fd < 0) {
        co_return nullptr;
    }

    auto stream = std::make_unique<IoUringTcpStream>(executor_);
    stream->InitFromFd(fd);
    co_return std::move(stream);
}

// ============================================================================
// IoUringTcpStream Implementation
// ============================================================================

IoUringTcpStream::IoUringTcpStream(IoUringExecutor& executor) : executor_(executor) {}

IoUringTcpStream::~IoUringTcpStream() {
    Close();
}

void IoUringTcpStream::InitFromFd(int fd) {
    if (fd_ >= 0) return;  // Already initialized
    fd_ = fd;
    closed_ = false;
}

void IoUringTcpStream::Close() {
    if (!closed_ && fd_ >= 0) {
        close(fd_);
        fd_ = -1;
        closed_ = true;
    }
}

Task<int> IoUringTcpStream::Connect(const std::string& host, uint16_t port) {
    if (fd_ >= 0) {
        co_return -EALREADY;
    }

    // Create socket
    fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd_ < 0) {
        co_return -errno;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close(fd_);
        fd_ = -1;
        co_return -EINVAL;
    }

    int32_t result = co_await IoUringConnect(fd_, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
    if (result < 0) {
        close(fd_);
        fd_ = -1;
        co_return result;
    }

    co_return 0;
}

Task<std::vector<char>> IoUringTcpStream::Read(size_t max_bytes) {
    if (!IsOpen()) {
        co_return std::vector<char>{};
    }
    std::vector<char> buf(max_bytes);
    int32_t result = co_await IoUringRecv(fd_, buf.data(), buf.size(), 0);
    if (result == 0) {
        // EOF — peer closed the connection
        closed_ = true;
        co_return std::vector<char>{};
    }
    if (result < 0) {
        // Error — don't mark closed, caller may retry or inspect
        co_return std::vector<char>{};
    }
    buf.resize(static_cast<size_t>(result));
    co_return std::move(buf);
}

Task<std::vector<char>> IoUringTcpStream::ReadProvided() {
    if (!IsOpen()) {
        co_return std::vector<char>{};
    }

    if (!executor_.GetConfig().provided_buffers) {
        // Fallback to normal read if provided buffers are disabled
        co_return co_await Read();
    }

    uint16_t bgid = executor_.GetConfig().buffer_group_id;
    auto result = co_await IoUringRecvProvided(fd_, bgid, 0);

    if (result.res == 0) {
        // EOF
        closed_ = true;
        co_return std::vector<char>{};
    }
    if (result.res < 0) {
        // Error
        co_return std::vector<char>{};
    }

    // A buffer was successfully grabbed. Get the buffer ID from the upper 16 bits of flags.
    if (!(result.flags & IORING_CQE_F_BUFFER)) {
        // Kernel completed recv but did not assign a provided buffer.
        // This should not happen with single-shot + IOSQE_BUFFER_SELECT,
        // but fall back to a normal read as a safety measure.
        co_return co_await Read();
    }

    uint16_t bid = result.flags >> 16;
    size_t bytes_read = static_cast<size_t>(result.res);

    // Retrieve the data from the executor's buffer ring
    char* base = executor_.GetBufferBase();
    char* buf_ptr = base + (bid * executor_.GetConfig().buffer_size);

    std::vector<char> data(buf_ptr, buf_ptr + bytes_read);

    // Return the buffer back to the ring
    executor_.ReturnBuffer(bgid, bid);

    co_return data;
}

Task<ssize_t> IoUringTcpStream::Write(std::string_view data) {
    if (!IsOpen()) {
        co_return -EBADF;
    }
    // Copy data to ensure lifetime across the co_await
    std::string owned(data);
    size_t total_sent = 0;
    while (total_sent < owned.size()) {
        int32_t result = co_await IoUringSend(fd_, owned.data() + total_sent, owned.size() - total_sent, MSG_NOSIGNAL);
        if (result <= 0) {
            // On error, return bytes already sent if any, otherwise the error
            co_return total_sent > 0 ? static_cast<ssize_t>(total_sent) : static_cast<ssize_t>(result);
        }
        total_sent += static_cast<size_t>(result);
    }
    co_return static_cast<ssize_t>(total_sent);
}

}  // namespace hotcoco

#endif  // HOTCOCO_HAS_IOURING
