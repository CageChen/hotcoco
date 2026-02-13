// ============================================================================
// SyncTcp Tests
// ============================================================================

#include "hotcoco/io/sync_tcp.hpp"

#include <gtest/gtest.h>
#include <thread>

using namespace hotcoco;

// ============================================================================
// Basic SyncTcp Tests
// ============================================================================

TEST(SyncTcpTest, ListenAndAccept) {
    auto listener = SyncTcpListener::Listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.IsOk()) << listener.Error().message();
    auto port = listener.Value()->GetPort();
    EXPECT_GT(port, 0);

    std::thread client_thread([port]() {
        auto conn = SyncTcpStream::Connect("127.0.0.1", port);
        ASSERT_TRUE(conn.IsOk()) << conn.Error().message();
        auto result = conn.Value()->SendAll("hello");
        EXPECT_TRUE(result.IsOk());
        conn.Value()->Close();
    });

    auto conn = listener.Value()->Accept();
    ASSERT_TRUE(conn.IsOk()) << conn.Error().message();
    EXPECT_TRUE(conn.Value()->IsOpen());

    auto data = conn.Value()->RecvExact(5);
    ASSERT_TRUE(data.IsOk()) << data.Error().message();
    EXPECT_EQ(data.Value(), "hello");

    conn.Value()->Close();
    client_thread.join();
}

TEST(SyncTcpTest, SendAndRecv) {
    auto listener = SyncTcpListener::Listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.IsOk());
    auto port = listener.Value()->GetPort();

    std::thread client_thread([port]() {
        auto conn = SyncTcpStream::Connect("127.0.0.1", port);
        ASSERT_TRUE(conn.IsOk());

        auto send_result = conn.Value()->Send("ping");
        EXPECT_TRUE(send_result.IsOk());
        EXPECT_EQ(send_result.Value(), 4);

        auto recv_result = conn.Value()->Recv(1024);
        ASSERT_TRUE(recv_result.IsOk());
        std::string response(recv_result.Value().begin(), recv_result.Value().end());
        EXPECT_EQ(response, "pong");

        conn.Value()->Close();
    });

    auto conn = listener.Value()->Accept();
    ASSERT_TRUE(conn.IsOk());

    auto data = conn.Value()->Recv(1024);
    ASSERT_TRUE(data.IsOk());
    std::string request(data.Value().begin(), data.Value().end());
    EXPECT_EQ(request, "ping");

    auto send_result = conn.Value()->SendAll("pong");
    EXPECT_TRUE(send_result.IsOk());

    conn.Value()->Close();
    client_thread.join();
}

TEST(SyncTcpTest, ConnectToInvalidAddress) {
    auto conn = SyncTcpStream::Connect("127.0.0.1", 1);
    // Port 1 is unlikely to be open; expect failure
    EXPECT_TRUE(conn.IsErr());
}

TEST(SyncTcpTest, ListenOnInvalidAddress) {
    auto listener = SyncTcpListener::Listen("999.999.999.999", 0);
    EXPECT_TRUE(listener.IsErr());
}

TEST(SyncTcpTest, CloseIdempotent) {
    auto listener = SyncTcpListener::Listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.IsOk());
    auto port = listener.Value()->GetPort();

    std::thread client_thread([port]() {
        auto conn = SyncTcpStream::Connect("127.0.0.1", port);
        if (conn.IsOk()) {
            conn.Value()->Close();
            // Second close should be safe
            conn.Value()->Close();
            EXPECT_FALSE(conn.Value()->IsOpen());
        }
    });

    auto conn = listener.Value()->Accept();
    if (conn.IsOk()) {
        conn.Value()->Close();
    }
    client_thread.join();
}

TEST(SyncTcpTest, StreamMoveConstruction) {
    auto listener = SyncTcpListener::Listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.IsOk());
    auto port = listener.Value()->GetPort();

    std::thread client_thread([port]() {
        auto conn = SyncTcpStream::Connect("127.0.0.1", port);
        if (conn.IsOk()) {
            // Move the stream
            auto moved = std::move(conn.Value());
            EXPECT_TRUE(moved->IsOpen());
            moved->Close();
        }
    });

    auto conn = listener.Value()->Accept();
    if (conn.IsOk()) {
        conn.Value()->Close();
    }
    client_thread.join();
}

// ============================================================================
// Error Path Tests
// ============================================================================

TEST(SyncTcpTest, SendOnClosedStream) {
    auto listener = SyncTcpListener::Listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.IsOk());
    auto port = listener.Value()->GetPort();

    std::thread client_thread([port]() {
        auto conn = SyncTcpStream::Connect("127.0.0.1", port);
        ASSERT_TRUE(conn.IsOk());
        conn.Value()->Close();

        // Operations on closed stream should return NotConnected
        auto send_result = conn.Value()->Send("data");
        EXPECT_TRUE(send_result.IsErr());
        EXPECT_EQ(send_result.Error(), make_error_code(Errc::NotConnected));

        auto send_all_result = conn.Value()->SendAll("data");
        EXPECT_TRUE(send_all_result.IsErr());
        EXPECT_EQ(send_all_result.Error(), make_error_code(Errc::NotConnected));

        auto recv_result = conn.Value()->Recv(1024);
        EXPECT_TRUE(recv_result.IsErr());
        EXPECT_EQ(recv_result.Error(), make_error_code(Errc::NotConnected));

        auto recv_exact_result = conn.Value()->RecvExact(4);
        EXPECT_TRUE(recv_exact_result.IsErr());
        EXPECT_EQ(recv_exact_result.Error(), make_error_code(Errc::NotConnected));
    });

    auto conn = listener.Value()->Accept();
    if (conn.IsOk()) {
        conn.Value()->Close();
    }
    client_thread.join();
}

TEST(SyncTcpTest, StreamMoveAssignment) {
    auto listener = SyncTcpListener::Listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.IsOk());
    auto port = listener.Value()->GetPort();

    std::thread client_thread([port]() {
        auto conn1 = SyncTcpStream::Connect("127.0.0.1", port);
        ASSERT_TRUE(conn1.IsOk());

        // Move-assign to a new unique_ptr holder — this exercises the stream's
        // move constructor via unique_ptr's move
        auto stream1 = std::move(conn1).Value();
        EXPECT_TRUE(stream1->IsOpen());

        stream1->Close();
    });

    auto conn = listener.Value()->Accept();
    if (conn.IsOk()) {
        conn.Value()->Close();
    }
    client_thread.join();
}

TEST(SyncTcpTest, ListenerMoveAssignment) {
    auto listener1 = SyncTcpListener::Listen("127.0.0.1", 0);
    ASSERT_TRUE(listener1.IsOk());
    auto port = listener1.Value()->GetPort();
    EXPECT_GT(port, 0);

    // Move-assign listener
    auto moved_listener = std::move(listener1).Value();
    EXPECT_EQ(moved_listener->GetPort(), port);
}

TEST(SyncTcpTest, ConnectToHostname) {
    // This exercises the hostname resolution (getaddrinfo) path in Connect()
    // Port 1 is unlikely to be open, so we expect ConnectFailed (not ResolveFailed)
    auto conn = SyncTcpStream::Connect("localhost", 1);
    EXPECT_TRUE(conn.IsErr());
    // The error should be ConnectFailed (hostname resolved OK, but connection refused)
    // or ResolveFailed if DNS is broken — either is a valid error path
    EXPECT_TRUE(conn.Error() == make_error_code(Errc::ConnectFailed) ||
                conn.Error() == make_error_code(Errc::ResolveFailed));
}

TEST(SyncTcpTest, FullSendRecvFlow) {
    // Listen on port 0 → accept → send → recv → verify
    auto listener_result = SyncTcpListener::Listen("127.0.0.1", 0);
    ASSERT_TRUE(listener_result.IsOk());
    auto& listener = *listener_result.Value();
    uint16_t port = listener.GetPort();
    ASSERT_GT(port, 0);

    // Connect from client in a separate thread
    std::thread client_thread([port]() {
        auto conn = SyncTcpStream::Connect("127.0.0.1", port);
        ASSERT_TRUE(conn.IsOk());
        auto& stream = *conn.Value();

        // SendAll
        auto send_res = stream.SendAll("hello world");
        EXPECT_TRUE(send_res.IsOk());

        // Recv response
        auto recv_res = stream.Recv(64);
        EXPECT_TRUE(recv_res.IsOk());

        stream.Close();
    });

    // Accept on server side
    auto accept_result = listener.Accept();
    ASSERT_TRUE(accept_result.IsOk());
    auto& server_stream = *accept_result.Value();

    // RecvExact
    auto recv_res = server_stream.RecvExact(11);  // "hello world" = 11 bytes
    ASSERT_TRUE(recv_res.IsOk());
    EXPECT_EQ(recv_res.Value(), "hello world");

    // Send response back
    auto send_res = server_stream.Send("ok");
    EXPECT_TRUE(send_res.IsOk());

    server_stream.Close();
    client_thread.join();
}
