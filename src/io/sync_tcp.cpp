// ============================================================================
// hotcoco/io/sync_tcp.cpp - Synchronous TCP Implementation
// ============================================================================

#include "hotcoco/io/sync_tcp.hpp"

#include <sys/socket.h>

#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <unistd.h>

namespace hotcoco {

// ============================================================================
// SyncTcpStream Implementation
// ============================================================================

SyncTcpStream::~SyncTcpStream() {
    Close();
}

SyncTcpStream::SyncTcpStream(SyncTcpStream&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

SyncTcpStream& SyncTcpStream::operator=(SyncTcpStream&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

Result<std::unique_ptr<SyncTcpStream>, std::error_code> SyncTcpStream::Connect(const std::string& host, uint16_t port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return Err(make_error_code(Errc::SocketCreateFailed));
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        // Try hostname resolution using thread-safe getaddrinfo()
        struct addrinfo hints{}, *result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        int err = getaddrinfo(host.c_str(), nullptr, &hints, &result);
        if (err != 0 || !result) {
            ::close(sockfd);
            return Err(make_error_code(Errc::ResolveFailed));
        }
        auto* sin = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
        addr.sin_addr = sin->sin_addr;
        freeaddrinfo(result);
    }

    if (::connect(sockfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(sockfd);
        return Err(make_error_code(Errc::ConnectFailed));
    }

    return Ok(std::unique_ptr<SyncTcpStream>(new SyncTcpStream(sockfd)));
}

Result<ssize_t, std::error_code> SyncTcpStream::Send(std::string_view data) {
    if (fd_ < 0) {
        return Err(make_error_code(Errc::NotConnected));
    }
    ssize_t n = ::send(fd_, data.data(), data.size(), 0);
    if (n < 0) {
        return Err(make_error_code(Errc::SendFailed));
    }
    return Ok(n);
}

Result<void, std::error_code> SyncTcpStream::SendAll(std::string_view data) {
    if (fd_ < 0) {
        return Err(make_error_code(Errc::NotConnected));
    }

    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            return Err(make_error_code(Errc::SendFailed));
        }
        sent += static_cast<size_t>(n);
    }
    return Ok();
}

Result<std::vector<char>, std::error_code> SyncTcpStream::Recv(size_t max_bytes) {
    if (fd_ < 0) {
        return Err(make_error_code(Errc::NotConnected));
    }

    std::vector<char> buf(max_bytes);
    ssize_t n = ::recv(fd_, buf.data(), max_bytes, 0);
    if (n < 0) {
        return Err(make_error_code(Errc::RecvFailed));
    }
    buf.resize(static_cast<size_t>(n));
    return Ok(std::move(buf));
}

Result<std::string, std::error_code> SyncTcpStream::RecvExact(size_t n) {
    if (fd_ < 0) {
        return Err(make_error_code(Errc::NotConnected));
    }

    std::string result;
    result.resize(n);

    size_t received = 0;
    while (received < n) {
        ssize_t r = ::recv(fd_, result.data() + received, n - received, 0);
        if (r <= 0) {
            return Err(make_error_code(Errc::RecvFailed));
        }
        received += static_cast<size_t>(r);
    }

    return Ok(std::move(result));
}

void SyncTcpStream::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// ============================================================================
// SyncTcpListener Implementation
// ============================================================================

SyncTcpListener::~SyncTcpListener() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

SyncTcpListener::SyncTcpListener(SyncTcpListener&& other) noexcept : fd_(other.fd_), port_(other.port_) {
    other.fd_ = -1;
    other.port_ = 0;
}

SyncTcpListener& SyncTcpListener::operator=(SyncTcpListener&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = other.fd_;
        port_ = other.port_;
        other.fd_ = -1;
        other.port_ = 0;
    }
    return *this;
}

Result<std::unique_ptr<SyncTcpListener>, std::error_code> SyncTcpListener::Listen(const std::string& host,
                                                                                  uint16_t port, int backlog) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return Err(make_error_code(Errc::SocketCreateFailed));
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (host == "0.0.0.0" || host.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            ::close(sockfd);
            return Err(make_error_code(Errc::InvalidAddress));
        }
    }

    if (bind(sockfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(sockfd);
        return Err(make_error_code(Errc::BindFailed));
    }

    if (listen(sockfd, backlog) < 0) {
        ::close(sockfd);
        return Err(make_error_code(Errc::ListenFailed));
    }

    // Get actual port if 0 was specified
    socklen_t len = sizeof(addr);
    getsockname(sockfd, reinterpret_cast<sockaddr*>(&addr), &len);

    auto listener = std::unique_ptr<SyncTcpListener>(new SyncTcpListener());
    listener->fd_ = sockfd;
    listener->port_ = ntohs(addr.sin_port);
    return Ok(std::move(listener));
}

Result<std::unique_ptr<SyncTcpStream>, std::error_code> SyncTcpListener::Accept() {
    if (fd_ < 0) {
        return Err(make_error_code(Errc::ListenerNotInitialized));
    }

    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    int client_fd = ::accept(fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
    if (client_fd < 0) {
        return Err(make_error_code(Errc::AcceptFailed));
    }

    return Ok(std::unique_ptr<SyncTcpStream>(new SyncTcpStream(client_fd)));
}

}  // namespace hotcoco
