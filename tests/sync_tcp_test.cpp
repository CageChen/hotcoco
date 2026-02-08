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
