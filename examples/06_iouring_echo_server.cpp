// ============================================================================
// Example 06: io_uring TCP Echo Server
// ============================================================================
//
// This example demonstrates async TCP networking using io_uring instead of
// libuv. It creates the same echo server as example 04, but powered by
// IoUringExecutor + IoUringTcpListener/IoUringTcpStream.
//
// RUN:
//   cd build && ./examples/06_iouring_echo_server
//
// TEST WITH:
//   echo "Hello!" | nc localhost 8889
//
// ============================================================================

#ifdef HOTCOCO_HAS_IOURING

#include <iostream>

#include "hotcoco/core/spawn.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/io/iouring_executor.hpp"
#include "hotcoco/io/iouring_tcp.hpp"
#include "hotcoco/io/timer.hpp"

using namespace hotcoco;
using namespace std::chrono_literals;

// Global state for demo shutdown
IoUringExecutor* g_executor = nullptr;
int g_connection_count = 0;
const int MAX_CONNECTIONS = 3;

// Handle a single client connection
Task<void> HandleClient(std::unique_ptr<IoUringTcpStream> client, int id) {
    std::cout << "[Client " << id << "] Connected" << std::endl;

    while (client->IsOpen()) {
        auto data = co_await client->Read();

        if (data.empty()) {
            std::cout << "[Client " << id << "] Disconnected" << std::endl;
            break;
        }

        std::string message(data.begin(), data.end());
        std::cout << "[Client " << id << "] Received: " << message;

        // Echo back with prefix
        co_await client->Write("Echo: " + message);
    }

    client->Close();
    g_connection_count++;

    if (g_connection_count >= MAX_CONNECTIONS) {
        std::cout << "\n[Server] Reached " << MAX_CONNECTIONS
                  << " connections, stopping..." << std::endl;
        g_executor->Stop();
    }
}

// Main server task
Task<void> RunServer(IoUringExecutor& executor) {
    IoUringTcpListener listener(executor);

    int bind_result = listener.Bind("0.0.0.0", 8889);
    if (bind_result != 0) {
        std::cout << "[Server] Failed to bind: " << bind_result << std::endl;
        executor.Stop();
        co_return;
    }

    int listen_result = listener.Listen();
    if (listen_result != 0) {
        std::cout << "[Server] Failed to listen: " << listen_result << std::endl;
        executor.Stop();
        co_return;
    }

    std::cout << "[Server] Listening on 0.0.0.0:8889 (io_uring backend)" << std::endl;
    std::cout << "[Server] Test with: echo 'Hello!' | nc localhost 8889" << std::endl;
    std::cout << "[Server] Will stop after " << MAX_CONNECTIONS << " connections" << std::endl;
    std::cout << std::endl;

    int client_id = 0;
    while (g_connection_count < MAX_CONNECTIONS) {
        auto client = co_await listener.Accept();
        if (client) {
            // Spawn handler — fire-and-forget via Spawn()
            Spawn(executor, HandleClient(std::move(client), ++client_id));
        }
    }
}

int main() {
    std::cout << "=== Hotcoco Example 06: io_uring TCP Echo Server ===" << std::endl;
    std::cout << std::endl;

    auto result = IoUringExecutor::Create();
    if (!result.IsOk()) {
        std::cerr << "Failed to create IoUringExecutor" << std::endl;
        return 1;
    }

    auto& executor = *result.Value();
    g_executor = &executor;

    auto server = RunServer(executor);
    executor.Schedule(server.GetHandle());
    executor.Run();

    std::cout << "\n=== Server stopped ===" << std::endl;
    return 0;
}

#else  // !HOTCOCO_HAS_IOURING

#include <iostream>

int main() {
    std::cerr << "This example requires io_uring support (liburing)." << std::endl;
    std::cerr << "Rebuild with -DENABLE_IOURING=ON and liburing installed." << std::endl;
    return 1;
}

#endif  // HOTCOCO_HAS_IOURING
