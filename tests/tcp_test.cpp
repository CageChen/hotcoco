// ============================================================================
// TCP Unit Tests
// ============================================================================

#include "hotcoco/io/tcp.hpp"

#include "hotcoco/core/task.hpp"
#include "hotcoco/io/libuv_executor.hpp"
#include "hotcoco/io/timer.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// TcpListener Tests
// ============================================================================

TEST(TcpTest, ListenerBindAndListen) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    TcpListener listener(executor);

    int bind_result = listener.Bind("127.0.0.1", 0);  // Port 0 = auto-assign
    EXPECT_EQ(bind_result, 0);

    int listen_result = listener.Listen();
    EXPECT_EQ(listen_result, 0);
}

TEST(TcpTest, ListenerBindInvalidAddress) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    TcpListener listener(executor);

    // Invalid IP address
    int result = listener.Bind("999.999.999.999", 8080);
    EXPECT_NE(result, 0);
}

// ============================================================================
// Echo Server Test
// ============================================================================

TEST(TcpTest, EchoServerClientCommunication) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    bool server_received = false;
    bool client_received = false;
    std::string received_data;

    // Server task
    auto server_task = [&]() -> Task<void> {
        TcpListener listener(executor);
        EXPECT_EQ(listener.Bind("127.0.0.1", 19876), 0);
        EXPECT_EQ(listener.Listen(), 0);

        // Wait for client connection
        auto stream = co_await listener.Accept();
        EXPECT_NE(stream, nullptr);

        // Read data
        auto data = co_await stream->Read();
        server_received = true;
        received_data = std::string(data.begin(), data.end());

        // Echo back
        co_await stream->Write(received_data);

        stream->Close();
    };

    // Client task (delayed to let server start)
    auto client_task = [&]() -> Task<void> {
        co_await AsyncSleep(50ms);  // Wait for server to start

        TcpStream client(executor);
        int result = co_await client.Connect("127.0.0.1", 19876);
        EXPECT_EQ(result, 0);

        // Send data
        co_await client.Write("Hello, hotcoco!");

        // Read echo
        auto response = co_await client.Read();
        client_received = !response.empty();

        client.Close();
        executor.Stop();
    };

    auto server = server_task();
    auto client = client_task();

    executor.Schedule(server.GetHandle());
    executor.Schedule(client.GetHandle());
    executor.Run();

    EXPECT_TRUE(server_received);
    EXPECT_TRUE(client_received);
    EXPECT_EQ(received_data, "Hello, hotcoco!");
}

// ============================================================================
// Bug regression: TcpListener/TcpStream destructor UAF
// ============================================================================
// Previously, TcpListener's destructor called uv_close() without waiting for
// the close callback, destroying the uv_tcp_t handle while libuv still
// referenced it. This test verifies the destructor completes cleanly.

TEST(TcpTest, ListenerDestructorNoUAF) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;

    {
        // Create, bind, listen, then immediately destroy
        TcpListener listener(executor);
        EXPECT_EQ(listener.Bind("127.0.0.1", 0), 0);
        EXPECT_EQ(listener.Listen(), 0);
        // Destructor runs here — must wait for uv_close to complete
    }

    // If the fix is wrong, ASan would catch UAF here
    // Verify the executor loop is still usable after listener destruction
    bool callback_ran = false;
    executor.Post([&]() {
        callback_ran = true;
        executor.Stop();
    });
    executor.Run();
    EXPECT_TRUE(callback_ran);
}

TEST(TcpTest, StreamDestructorNoUAF) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    bool connected = false;

    // Set up a listener so the client can connect
    TcpListener listener(executor);
    EXPECT_EQ(listener.Bind("127.0.0.1", 19877), 0);
    EXPECT_EQ(listener.Listen(), 0);

    auto task = [&]() -> Task<void> {
        // Client connects then immediately goes out of scope
        {
            TcpStream client(executor);
            int result = co_await client.Connect("127.0.0.1", 19877);
            EXPECT_EQ(result, 0);
            connected = true;
            // Destructor runs here — must wait for uv_close to complete
        }
        executor.Stop();
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();

    EXPECT_TRUE(connected);
}

// ============================================================================
// Bug regression: AllocBuffer must not invalidate pointers via resize()
// ============================================================================
// Previously, AllocBuffer called resize() which could reallocate the vector,
// invalidating pointers libuv held from prior calls. The fix uses reserve()
// to ensure stability. This test exercises the read path to verify no crash.

TEST(TcpTest, ReadLargeDataNoCorruption) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    bool success = false;
    const std::string test_data(8192, 'X');  // Large payload to trigger realloc

    auto server_task = [&]() -> Task<void> {
        TcpListener listener(executor);
        EXPECT_EQ(listener.Bind("127.0.0.1", 19878), 0);
        EXPECT_EQ(listener.Listen(), 0);

        auto stream = co_await listener.Accept();
        EXPECT_NE(stream, nullptr);

        auto data = co_await stream->Read(16384);
        std::string received(data.begin(), data.end());
        success = (received == test_data);

        stream->Close();
    };

    auto client_task = [&]() -> Task<void> {
        co_await AsyncSleep(50ms);

        TcpStream client(executor);
        int result = co_await client.Connect("127.0.0.1", 19878);
        EXPECT_EQ(result, 0);

        co_await client.Write(test_data);
        client.Close();

        co_await AsyncSleep(50ms);
        executor.Stop();
    };

    auto server = server_task();
    auto client = client_task();
    executor.Schedule(server.GetHandle());
    executor.Schedule(client.GetHandle());
    executor.Run();

    EXPECT_TRUE(success);
}

// ============================================================================
// Bug regression: SyncTcpStream must use thread-safe name resolution
// ============================================================================
// Previously used gethostbyname() which returns a pointer to static data
// and is not thread-safe. Fixed by using getaddrinfo().

#include "hotcoco/io/sync_tcp.hpp"

TEST(TcpTest, SyncTcpConnectToLocalhost) {
    // Start a listener on a random port
    auto listener_result = SyncTcpListener::Listen("127.0.0.1", 0);
    ASSERT_TRUE(listener_result.IsOk());
    auto& listener = listener_result.Value();
    uint16_t port = listener->GetPort();

    // Connect using "127.0.0.1" (exercises getaddrinfo path indirectly)
    std::thread client_thread([port]() {
        auto stream_result = SyncTcpStream::Connect("127.0.0.1", port);
        ASSERT_TRUE(stream_result.IsOk());
        auto& stream = stream_result.Value();
        EXPECT_TRUE(stream->IsOpen());
        stream->SendAll("hello");
        stream->Close();
    });

    auto conn_result = listener->Accept();
    ASSERT_TRUE(conn_result.IsOk());
    auto& conn = conn_result.Value();
    auto data_result = conn->Recv(1024);
    ASSERT_TRUE(data_result.IsOk());
    auto& data = data_result.Value();
    std::string received(data.begin(), data.end());
    EXPECT_EQ(received, "hello");

    client_thread.join();
}

// ============================================================================
// Bug regression: ConnectAwaitable::await_resume returns wrong variable
// ============================================================================
// Previously, await_resume returned ConnectAwaitable::result_ which was only
// set on synchronous errors. For async completions, OnConnect wrote to
// TcpStream::connect_result_ but await_resume never read it — always
// returning 0 (success) even on connection failure.

TEST(TcpTest, ConnectFailureReportsError) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    int connect_result = 0;

    auto task = [&]() -> Task<void> {
        TcpStream client(executor);
        // Connect to a port where nothing is listening — should fail
        connect_result = co_await client.Connect("127.0.0.1", 19899);
        executor.Stop();
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();

    // Must report a non-zero error code (ECONNREFUSED or similar)
    EXPECT_NE(connect_result, 0) << "Async connect failure must be reported via await_resume";
}

// ============================================================================
// Bug regression: TcpStream destructor on uninitialized handle
// ============================================================================
// Previously, destroying a TcpStream that was never connected or accepted
// called uv_close on a handle that was only memset'd but never uv_tcp_init'd,
// causing a libuv assertion failure. The fix tracks initialization state.

TEST(TcpTest, DestroyUninitializedStreamNoUAF) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;

    {
        // Create and immediately destroy without connecting or accepting
        TcpStream stream(executor);
        // Destructor runs here — must not call uv_close on uninitialized handle
    }

    // If the fix is wrong, ASan/libuv would catch the error.
    // Verify the executor is still usable.
    bool callback_ran = false;
    executor.Post([&]() {
        callback_ran = true;
        executor.Stop();
    });
    executor.Run();
    EXPECT_TRUE(callback_ran);
}
