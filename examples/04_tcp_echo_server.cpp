// ============================================================================
// Example 04: TCP Echo Server
// ============================================================================
//
// This example demonstrates async TCP networking with hotcoco.
// It creates a simple echo server that:
// 1. Listens for connections
// 2. Reads data from clients
// 3. Echoes it back
//
// RUN:
//   cd build && ./examples/04_tcp_echo_server
//
// TEST WITH:
//   echo "Hello!" | nc localhost 8888
//
// ============================================================================

#include "hotcoco/core/task.hpp"
#include "hotcoco/io/libuv_executor.hpp"
#include "hotcoco/io/tcp.hpp"
#include "hotcoco/io/timer.hpp"

#include <iostream>

using namespace hotcoco;
using namespace std::chrono_literals;

// Global executor for stopping
LibuvExecutor* g_executor = nullptr;
int g_connection_count = 0;
const int MAX_CONNECTIONS = 3;  // Stop after 3 connections for demo

// Handle a single client connection
Task<void> HandleClient(std::unique_ptr<TcpStream> client, int id) {
    std::cout << "[Client " << id << "] Connected" << std::endl;

    while (client->IsOpen()) {
        // Read data from client
        auto data = co_await client->Read();

        if (data.empty()) {
            std::cout << "[Client " << id << "] Disconnected" << std::endl;
            break;
        }

        std::string message(data.begin(), data.end());
        std::cout << "[Client " << id << "] Received: " << message;

        // Echo back
        co_await client->Write("Echo: " + message);
    }

    client->Close();
    g_connection_count++;

    if (g_connection_count >= MAX_CONNECTIONS) {
        std::cout << "\n[Server] Reached " << MAX_CONNECTIONS << " connections, stopping..." << std::endl;
        g_executor->Stop();
    }
}

// Main server task
Task<void> RunServer(LibuvExecutor& executor) {
    TcpListener listener(executor);

    int bind_result = listener.Bind("127.0.0.1", 8888);
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

    std::cout << "[Server] Listening on 127.0.0.1:8888" << std::endl;
    std::cout << "[Server] Test with: echo 'Hello!' | nc localhost 8888" << std::endl;
    std::cout << "[Server] Will stop after " << MAX_CONNECTIONS << " connections" << std::endl;
    std::cout << std::endl;

    int client_id = 0;
    while (g_connection_count < MAX_CONNECTIONS) {
        auto client = co_await listener.Accept();
        if (client) {
            // Spawn handler task for this client
            auto handler = HandleClient(std::move(client), ++client_id);
            executor.Schedule(handler.GetHandle());
        }
    }
}

int main() {
    std::cout << "=== Hotcoco Example 04: TCP Echo Server ===" << std::endl;
    std::cout << std::endl;

    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    g_executor = &executor;

    auto server = RunServer(executor);
    executor.Schedule(server.GetHandle());
    executor.Run();

    std::cout << "\n=== Server stopped ===" << std::endl;
    return 0;
}
