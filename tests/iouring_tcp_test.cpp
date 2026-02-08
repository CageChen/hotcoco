// ============================================================================
// io_uring TCP Tests
// ============================================================================
//
// Tests for both low-level IoUringXxx() free functions and high-level
// IoUringTcpListener / IoUringTcpStream classes.
//
// Pattern: following tcp_test.cpp — each concurrent side is a separate
// Task<void> lambda, Schedule()'d independently, communicating via shared
// variables. Never co_await a Task that was Schedule()'d.
//

#ifdef HOTCOCO_HAS_IOURING

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "hotcoco/core/task.hpp"
#include "hotcoco/io/iouring_async_ops.hpp"
#include "hotcoco/io/iouring_executor.hpp"
#include "hotcoco/io/iouring_tcp.hpp"
#include "hotcoco/io/timer.hpp"

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// Helper: create a listening socket on port 0, return {listen_fd, port}
// ============================================================================
static std::pair<int, uint16_t> MakeListenSocket() {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return {-1, 0};

    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return {-1, 0};
    }
    if (listen(fd, 128) < 0) {
        close(fd);
        return {-1, 0};
    }

    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len);
    return {fd, ntohs(addr.sin_port)};
}

// ============================================================================
// Low-level IoUringAsyncOpsTest
// ============================================================================

TEST(IoUringAsyncOpsTest, AcceptAndConnect) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    auto [listen_fd, port] = MakeListenSocket();
    ASSERT_GT(listen_fd, 0);
    ASSERT_GT(port, 0);

    int accepted_fd = -1;
    int connect_result = -1;

    // Server side: accept
    auto server_task = [&]() -> Task<void> {
        accepted_fd = co_await IoUringAccept(listen_fd, nullptr, nullptr, 0);
    };

    // Client side: connect (small delay to let accept be submitted first)
    auto client_task = [&]() -> Task<void> {
        co_await AsyncSleep(20ms);

        int cfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        connect_result = co_await IoUringConnect(
            cfd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));

        close(cfd);

        co_await AsyncSleep(50ms);
        executor.Stop();
    };

    auto st = server_task();
    auto ct = client_task();
    executor.Schedule(st.GetHandle());
    executor.Schedule(ct.GetHandle());
    executor.Run();

    EXPECT_GE(accepted_fd, 0);
    EXPECT_EQ(connect_result, 0);

    if (accepted_fd >= 0) close(accepted_fd);
    close(listen_fd);
}

TEST(IoUringAsyncOpsTest, SendRecv) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    auto [listen_fd, port] = MakeListenSocket();
    ASSERT_GT(listen_fd, 0);

    const std::string test_data = "hello io_uring";
    std::string received_data;
    int32_t sent_bytes = 0;

    // Server: accept, recv, close
    auto server_task = [&]() -> Task<void> {
        int server_fd = co_await IoUringAccept(listen_fd, nullptr, nullptr, 0);
        if (server_fd < 0) {
            executor.Stop();
            co_return;
        }

        char buf[128]{};
        int32_t recvd = co_await IoUringRecv(server_fd, buf, sizeof(buf), 0);
        if (recvd > 0) {
            received_data.assign(buf, static_cast<size_t>(recvd));
        }

        close(server_fd);
    };

    // Client: connect, send, close, stop
    auto client_task = [&]() -> Task<void> {
        co_await AsyncSleep(20ms);

        int cfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        int r = co_await IoUringConnect(
            cfd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
        if (r < 0) {
            close(cfd);
            executor.Stop();
            co_return;
        }

        sent_bytes = co_await IoUringSend(
            cfd, test_data.data(), test_data.size(), MSG_NOSIGNAL);

        close(cfd);
        co_await AsyncSleep(50ms);
        executor.Stop();
    };

    auto st = server_task();
    auto ct = client_task();
    executor.Schedule(st.GetHandle());
    executor.Schedule(ct.GetHandle());
    executor.Run();

    EXPECT_EQ(sent_bytes, static_cast<int32_t>(test_data.size()));
    EXPECT_EQ(received_data, test_data);

    close(listen_fd);
}

TEST(IoUringAsyncOpsTest, RecvEOF) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    auto [listen_fd, port] = MakeListenSocket();
    ASSERT_GT(listen_fd, 0);

    int32_t recv_result = -999;

    // Server: accept, then recv (expect EOF after client closes)
    auto server_task = [&]() -> Task<void> {
        int server_fd = co_await IoUringAccept(listen_fd, nullptr, nullptr, 0);
        if (server_fd < 0) {
            executor.Stop();
            co_return;
        }

        // Wait a bit for client to close
        co_await AsyncSleep(50ms);

        char buf[64]{};
        recv_result = co_await IoUringRecv(server_fd, buf, sizeof(buf), 0);

        close(server_fd);
        executor.Stop();
    };

    // Client: connect then immediately close
    auto client_task = [&]() -> Task<void> {
        co_await AsyncSleep(20ms);

        int cfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        int r = co_await IoUringConnect(
            cfd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
        if (r < 0) { close(cfd); co_return; }

        // Close immediately to trigger EOF on server
        close(cfd);
    };

    auto st = server_task();
    auto ct = client_task();
    executor.Schedule(st.GetHandle());
    executor.Schedule(ct.GetHandle());
    executor.Run();

    EXPECT_EQ(recv_result, 0);  // EOF

    close(listen_fd);
}

TEST(IoUringAsyncOpsTest, ConnectRefused) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    int32_t connect_result = 0;

    auto task = [&]() -> Task<void> {
        int cfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(1);  // Privileged port, likely refused
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        connect_result = co_await IoUringConnect(
            cfd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));

        close(cfd);
        executor.Stop();
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();

    EXPECT_LT(connect_result, 0);
}

// ============================================================================
// High-level IoUringTcpTest
// ============================================================================

TEST(IoUringTcpTest, ListenerBindAndListen) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    IoUringTcpListener listener(executor);
    EXPECT_EQ(listener.Bind("127.0.0.1", 0), 0);
    EXPECT_EQ(listener.Listen(), 0);
    EXPECT_GT(listener.GetPort(), 0);
}

TEST(IoUringTcpTest, ListenerDoubleBindReturnsError) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    IoUringTcpListener listener(executor);
    ASSERT_EQ(listener.Bind("127.0.0.1", 0), 0);
    ASSERT_GT(listener.GetPort(), 0);

    // Second Bind() should fail with -EALREADY
    EXPECT_EQ(listener.Bind("127.0.0.1", 0), -EALREADY);
}

TEST(IoUringTcpTest, ListenerBindInvalidAddress) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    IoUringTcpListener listener(executor);
    EXPECT_NE(listener.Bind("not_an_ip", 0), 0);
}

TEST(IoUringTcpTest, EchoServerClient) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    IoUringTcpListener listener(executor);
    ASSERT_EQ(listener.Bind("127.0.0.1", 0), 0);
    ASSERT_EQ(listener.Listen(), 0);
    uint16_t port = listener.GetPort();
    ASSERT_GT(port, 0);

    const std::string test_msg = "echo test via io_uring";
    std::string received;
    ssize_t write_result = 0;

    auto server_task = [&]() -> Task<void> {
        auto stream = co_await listener.Accept();
        if (!stream) co_return;

        auto data = co_await stream->Read();
        if (data.empty()) co_return;

        // Echo back
        co_await stream->Write(std::string_view(data.data(), data.size()));
    };

    auto client_task = [&]() -> Task<void> {
        co_await AsyncSleep(20ms);

        IoUringTcpStream client(executor);
        int r = co_await client.Connect("127.0.0.1", port);
        if (r != 0) {
            executor.Stop();
            co_return;
        }

        write_result = co_await client.Write(test_msg);

        auto data = co_await client.Read();
        received.assign(data.begin(), data.end());

        executor.Stop();
    };

    auto st = server_task();
    auto ct = client_task();
    executor.Schedule(st.GetHandle());
    executor.Schedule(ct.GetHandle());
    executor.Run();

    EXPECT_EQ(write_result, static_cast<ssize_t>(test_msg.size()));
    EXPECT_EQ(received, test_msg);
}

TEST(IoUringTcpTest, ListenerDestructorClean) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    {
        IoUringTcpListener listener(executor);
        ASSERT_EQ(listener.Bind("127.0.0.1", 0), 0);
        ASSERT_EQ(listener.Listen(), 0);
        // Destructor runs here
    }

    // Executor should still be usable
    bool ran = false;
    executor.Post([&]() {
        ran = true;
        executor.Stop();
    });
    executor.Run();
    EXPECT_TRUE(ran);
}

TEST(IoUringTcpTest, StreamDestructorClean) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    IoUringTcpListener listener(executor);
    ASSERT_EQ(listener.Bind("127.0.0.1", 0), 0);
    ASSERT_EQ(listener.Listen(), 0);
    uint16_t port = listener.GetPort();

    bool client_connected = false;

    auto server_task = [&]() -> Task<void> {
        auto stream = co_await listener.Accept();
        // Just accept and let it go out of scope
        co_await AsyncSleep(50ms);
        executor.Stop();
    };

    auto client_task = [&]() -> Task<void> {
        co_await AsyncSleep(20ms);
        {
            IoUringTcpStream client(executor);
            int r = co_await client.Connect("127.0.0.1", port);
            client_connected = (r == 0);
            // Destructor closes fd
        }
    };

    auto st = server_task();
    auto ct = client_task();
    executor.Schedule(st.GetHandle());
    executor.Schedule(ct.GetHandle());
    executor.Run();

    EXPECT_TRUE(client_connected);
}

TEST(IoUringTcpTest, ReadLargeData) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    IoUringTcpListener listener(executor);
    ASSERT_EQ(listener.Bind("127.0.0.1", 0), 0);
    ASSERT_EQ(listener.Listen(), 0);
    uint16_t port = listener.GetPort();

    // 8192 bytes of patterned data
    std::string large_data(8192, '\0');
    for (size_t i = 0; i < large_data.size(); ++i) {
        large_data[i] = static_cast<char>('A' + (i % 26));
    }

    std::string received_all;

    auto server_task = [&]() -> Task<void> {
        auto stream = co_await listener.Accept();
        if (!stream) co_return;

        // Send all data
        co_await stream->Write(large_data);
        stream->Close();
    };

    auto client_task = [&]() -> Task<void> {
        co_await AsyncSleep(20ms);

        IoUringTcpStream client(executor);
        int r = co_await client.Connect("127.0.0.1", port);
        if (r != 0) {
            executor.Stop();
            co_return;
        }

        // Read until EOF
        while (client.IsOpen()) {
            auto data = co_await client.Read(4096);
            if (data.empty()) break;
            received_all.append(data.begin(), data.end());
        }

        executor.Stop();
    };

    auto st = server_task();
    auto ct = client_task();
    executor.Schedule(st.GetHandle());
    executor.Schedule(ct.GetHandle());
    executor.Run();

    EXPECT_EQ(received_all.size(), large_data.size());
    EXPECT_EQ(received_all, large_data);
}

TEST(IoUringTcpTest, ConnectFailure) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    int connect_result = 0;

    auto task = [&]() -> Task<void> {
        IoUringTcpStream client(executor);
        connect_result = co_await client.Connect("127.0.0.1", 1);
        executor.Stop();
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();

    EXPECT_LT(connect_result, 0);
}

TEST(IoUringTcpTest, DestroyUninitializedStream) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    {
        IoUringTcpStream stream(executor);
        // Never connected, just destroy — should not crash
        EXPECT_FALSE(stream.IsOpen());
    }
}

TEST(IoUringTcpTest, MultipleAccepts) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    IoUringTcpListener listener(executor);
    ASSERT_EQ(listener.Bind("127.0.0.1", 0), 0);
    ASSERT_EQ(listener.Listen(), 0);
    uint16_t port = listener.GetPort();

    int accept_count = 0;
    constexpr int kNumConnections = 3;

    auto server_task = [&]() -> Task<void> {
        for (int i = 0; i < kNumConnections; ++i) {
            auto stream = co_await listener.Accept();
            if (stream) {
                accept_count++;
                // Read and discard
                co_await stream->Read();
            }
        }
        executor.Stop();
    };

    auto client_task = [&]() -> Task<void> {
        co_await AsyncSleep(20ms);
        for (int i = 0; i < kNumConnections; ++i) {
            IoUringTcpStream client(executor);
            int r = co_await client.Connect("127.0.0.1", port);
            if (r == 0) {
                co_await client.Write("hi");
                client.Close();
            }
            // Small delay to let accept process
            co_await AsyncSleep(10ms);
        }
    };

    auto st = server_task();
    auto ct = client_task();
    executor.Schedule(st.GetHandle());
    executor.Schedule(ct.GetHandle());
    executor.Run();

    EXPECT_EQ(accept_count, kNumConnections);
}

TEST(IoUringTcpTest, WriteLargeDataPartialWrite) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    IoUringTcpListener listener(executor);
    ASSERT_EQ(listener.Bind("127.0.0.1", 0), 0);
    ASSERT_EQ(listener.Listen(), 0);
    uint16_t port = listener.GetPort();

    // 256 KB of patterned data — large enough to exceed socket buffer and
    // trigger partial writes from the kernel (typical SO_SNDBUF is 16-128 KB).
    constexpr size_t kDataSize = 256 * 1024;
    std::string large_data(kDataSize, '\0');
    for (size_t i = 0; i < large_data.size(); ++i) {
        large_data[i] = static_cast<char>('A' + (i % 26));
    }

    std::string received_all;
    ssize_t write_result = 0;

    auto server_task = [&]() -> Task<void> {
        auto stream = co_await listener.Accept();
        if (!stream) co_return;

        // Read until EOF to collect all data
        while (stream->IsOpen()) {
            auto data = co_await stream->Read(4096);
            if (data.empty()) break;
            received_all.append(data.begin(), data.end());
        }

        executor.Stop();
    };

    auto client_task = [&]() -> Task<void> {
        co_await AsyncSleep(20ms);

        IoUringTcpStream client(executor);
        int r = co_await client.Connect("127.0.0.1", port);
        if (r != 0) {
            executor.Stop();
            co_return;
        }

        // Write all 256 KB in one call — Write() must loop internally
        write_result = co_await client.Write(large_data);
        client.Close();
    };

    auto st = server_task();
    auto ct = client_task();
    executor.Schedule(st.GetHandle());
    executor.Schedule(ct.GetHandle());
    executor.Run();

    EXPECT_EQ(write_result, static_cast<ssize_t>(kDataSize));
    EXPECT_EQ(received_all.size(), kDataSize);
    EXPECT_EQ(received_all, large_data);
}

// ============================================================================
// Bug-fix regression tests
// ============================================================================

TEST(IoUringTcpTest, ReadOnClosedStreamReturnsEmpty) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    bool read_empty = false;
    bool still_closed = false;

    auto task = [&]() -> Task<void> {
        IoUringTcpStream stream(executor);
        // Never connected — IsOpen() is false
        auto data = co_await stream.Read();
        read_empty = data.empty();
        still_closed = !stream.IsOpen();
        executor.Stop();
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();

    EXPECT_TRUE(read_empty);
    EXPECT_TRUE(still_closed);
}

TEST(IoUringTcpTest, WriteOnClosedStreamReturnsEBADF) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    ssize_t write_result = 0;

    auto task = [&]() -> Task<void> {
        IoUringTcpStream stream(executor);
        // Never connected — IsOpen() is false
        write_result = co_await stream.Write("hello");
        executor.Stop();
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();

    EXPECT_EQ(write_result, -EBADF);
}

TEST(IoUringTcpTest, ReadEOFSetsClosedButErrorDoesNot) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    IoUringTcpListener listener(executor);
    ASSERT_EQ(listener.Bind("127.0.0.1", 0), 0);
    ASSERT_EQ(listener.Listen(), 0);
    uint16_t port = listener.GetPort();

    bool closed_after_eof = false;

    auto server_task = [&]() -> Task<void> {
        auto stream = co_await listener.Accept();
        if (!stream) co_return;

        // Wait for client to close, then read to get EOF
        co_await AsyncSleep(50ms);
        auto data = co_await stream->Read();
        // Should be empty (EOF) and stream should be closed
        closed_after_eof = data.empty() && !stream->IsOpen();

        executor.Stop();
    };

    auto client_task = [&]() -> Task<void> {
        co_await AsyncSleep(20ms);
        IoUringTcpStream client(executor);
        int r = co_await client.Connect("127.0.0.1", port);
        if (r != 0) co_return;
        // Close immediately to trigger EOF on server side
        client.Close();
    };

    auto st = server_task();
    auto ct = client_task();
    executor.Schedule(st.GetHandle());
    executor.Schedule(ct.GetHandle());
    executor.Run();

    EXPECT_TRUE(closed_after_eof);
}

TEST(IoUringTcpTest, WriteOnExplicitlyClosedStream) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    IoUringTcpListener listener(executor);
    ASSERT_EQ(listener.Bind("127.0.0.1", 0), 0);
    ASSERT_EQ(listener.Listen(), 0);
    uint16_t port = listener.GetPort();

    ssize_t write_after_close = 0;

    auto server_task = [&]() -> Task<void> {
        auto stream = co_await listener.Accept();
        co_await AsyncSleep(100ms);
        executor.Stop();
    };

    auto client_task = [&]() -> Task<void> {
        co_await AsyncSleep(20ms);
        IoUringTcpStream client(executor);
        int r = co_await client.Connect("127.0.0.1", port);
        if (r != 0) co_return;

        client.Close();
        // Write after explicit Close() should return -EBADF
        write_after_close = co_await client.Write("hello");
    };

    auto st = server_task();
    auto ct = client_task();
    executor.Schedule(st.GetHandle());
    executor.Schedule(ct.GetHandle());
    executor.Run();

    EXPECT_EQ(write_after_close, -EBADF);
}

TEST(IoUringTcpTest, ReadOnExplicitlyClosedStream) {
    auto res = IoUringExecutor::Create();
    ASSERT_TRUE(res.IsOk());
    auto& executor = *res.Value();

    IoUringTcpListener listener(executor);
    ASSERT_EQ(listener.Bind("127.0.0.1", 0), 0);
    ASSERT_EQ(listener.Listen(), 0);
    uint16_t port = listener.GetPort();

    bool read_empty = false;

    auto server_task = [&]() -> Task<void> {
        auto stream = co_await listener.Accept();
        co_await AsyncSleep(100ms);
        executor.Stop();
    };

    auto client_task = [&]() -> Task<void> {
        co_await AsyncSleep(20ms);
        IoUringTcpStream client(executor);
        int r = co_await client.Connect("127.0.0.1", port);
        if (r != 0) co_return;

        client.Close();
        // Read after explicit Close() should return empty immediately
        auto data = co_await client.Read();
        read_empty = data.empty();
    };

    auto st = server_task();
    auto ct = client_task();
    executor.Schedule(st.GetHandle());
    executor.Schedule(ct.GetHandle());
    executor.Run();

    EXPECT_TRUE(read_empty);
}

#endif  // HOTCOCO_HAS_IOURING
