// ============================================================================
// TCP Benchmarks
// ============================================================================
//
// Benchmarks for io_uring async TCP vs synchronous TCP baselines.
// Tests round-trip latency and throughput in same-core and cross-core modes.
//
// Run with:
//   ./build/benchmarks/hotcoco_bench --benchmark_filter="Tcp"
//
// ----------------------------------------------------------------------------
// Reference results (Release, 32-core, 5.7 GHz):
//
//   Round-trip latency (io_uring vs sync, single connection):
//                  uring-same   uring-cross  sync         winner
//     64B msg:      3.3 us       6.1 us       3.3 us       tie
//     1KB msg:      3.4 us       6.7 us       3.4 us       tie
//     4KB msg:      3.7 us       6.5 us       3.7 us       tie
//
//   Throughput (io_uring vs sync, single connection):
//                  uring-same   uring-cross  sync         winner
//     4KB msg:     1.4 GiB/s     2.8 GiB/s   17.7 GiB/s   sync 6.3x
//     64KB msg:   11.0 GiB/s    16.2 GiB/s   44.5 GiB/s   sync 2.7x
//
//   Multi-connection echo (io_uring 1 thread vs sync N threads):
//                  io_uring       sync           speedup
//     4 conns:     10.6 us        25.0 us        2.3x
//     16 conns:    39.4 us        132  us        3.3x
//     64 conns:    159  us        661  us        4.1x
//     256 conns:   700  us        2051 us        2.9x
// ============================================================================

#ifdef HOTCOCO_HAS_IOURING

#include "hotcoco/core/task.hpp"
#include "hotcoco/core/when_all.hpp"
#include "hotcoco/io/iouring_executor.hpp"
#include "hotcoco/io/iouring_tcp.hpp"
#include "hotcoco/io/sync_tcp.hpp"
#include "hotcoco/io/thread_utils.hpp"
#include "hotcoco/io/timer.hpp"

#include <atomic>
#include <barrier>
#include <benchmark/benchmark.h>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace hotcoco;
using namespace std::chrono_literals;

namespace {

// ============================================================================
// Helpers
// ============================================================================

// Run a Task<void> coroutine to completion on an IoUringExecutor.
// The coroutine MUST call executor->Stop() before co_return.
void RunTask(IoUringExecutor& executor, Task<void> task) {
    executor.Schedule(task.GetHandle());
    executor.Run();
}

// ============================================================================
// SameCore TCP Fixture: both server and client on one executor
// ============================================================================

class TcpSameCoreFixture : public benchmark::Fixture {
   public:
    void SetUp(benchmark::State&) override {
        auto res = IoUringExecutor::Create();
        executor_ = std::move(res).Value();

        listener_ = std::make_unique<IoUringTcpListener>(*executor_);
        (void)listener_->Bind("127.0.0.1", 0);
        (void)listener_->Listen();
        auto port = listener_->GetPort();

        client_stream_ = std::make_unique<IoUringTcpStream>(*executor_);

        // Schedule accept and connect as independent tasks.
        // Both Task objects must outlive executor_->Run().
        auto accept_task = [this]() -> Task<void> { server_stream_ = co_await listener_->Accept(); };

        auto connect_task = [this, port]() -> Task<void> {
            co_await AsyncSleep(5ms);
            co_await client_stream_->Connect("127.0.0.1", port);
            co_await AsyncSleep(10ms);
            executor_->Stop();
        };

        auto at = accept_task();
        auto ct = connect_task();
        executor_->Schedule(at.GetHandle());
        executor_->Schedule(ct.GetHandle());
        executor_->Run();
    }

    void TearDown(benchmark::State&) override {
        if (client_stream_) {
            client_stream_->Close();
            client_stream_.reset();
        }
        if (server_stream_) {
            server_stream_->Close();
            server_stream_.reset();
        }
        listener_.reset();
        executor_.reset();
    }

   protected:
    std::unique_ptr<IoUringExecutor> executor_;
    std::unique_ptr<IoUringTcpListener> listener_;
    std::unique_ptr<IoUringTcpStream> server_stream_;
    std::unique_ptr<IoUringTcpStream> client_stream_;
};

// --- SameCore Round-Trip ---
BENCHMARK_DEFINE_F(TcpSameCoreFixture, BM_IoUringTcpRoundTrip_SameCore)
(benchmark::State& state) {
    const auto msg_size = static_cast<size_t>(state.range(0));
    std::string send_data(msg_size, 'R');

    for (auto _ : state) {
        auto bench_task = [&]() -> Task<void> {
            co_await client_stream_->Write(send_data);
            auto data = co_await server_stream_->Read(msg_size);
            benchmark::DoNotOptimize(data);
            co_await server_stream_->Write(std::string_view(data.data(), data.size()));
            auto echo = co_await client_stream_->Read(msg_size);
            benchmark::DoNotOptimize(echo);
            executor_->Stop();
        };
        RunTask(*executor_, bench_task());
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(msg_size) * 2);
}
BENCHMARK_REGISTER_F(TcpSameCoreFixture, BM_IoUringTcpRoundTrip_SameCore)->Arg(64)->Arg(1024)->Arg(4096);

// --- SameCore Throughput ---
BENCHMARK_DEFINE_F(TcpSameCoreFixture, BM_IoUringTcpThroughput_SameCore)
(benchmark::State& state) {
    const auto block_size = static_cast<size_t>(state.range(0));
    std::string send_data(block_size, 'T');

    for (auto _ : state) {
        auto bench_task = [&]() -> Task<void> {
            co_await client_stream_->Write(send_data);
            size_t total_read = 0;
            while (total_read < block_size) {
                auto data = co_await server_stream_->Read(block_size - total_read);
                total_read += data.size();
                benchmark::DoNotOptimize(data);
            }
            executor_->Stop();
        };
        RunTask(*executor_, bench_task());
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(block_size));
}
BENCHMARK_REGISTER_F(TcpSameCoreFixture, BM_IoUringTcpThroughput_SameCore)->Arg(4096)->Arg(65536);

// ============================================================================
// CrossCore TCP Round-Trip: server on separate thread
// ============================================================================

static void BM_IoUringTcpRoundTrip_CrossCore(benchmark::State& state) {
    const auto msg_size = static_cast<size_t>(state.range(0));
    std::string send_data(msg_size, 'X');

    int num_cpus = GetNumCpus();
    int client_cpu = 0;
    int server_cpu = (num_cpus > 1) ? 1 : 0;

    IoUringExecutor::Config config;
    config.provided_buffers = true;
    config.buffer_ring_size = 4096;

    // Client executor on benchmark thread
    auto client_res = IoUringExecutor::Create(config);
    auto& client_executor = *client_res.Value();

    // Server executor on separate thread
    auto server_res = IoUringExecutor::Create(config);
    auto& server_executor = *server_res.Value();

    // Set up listener on server executor
    IoUringTcpListener listener(server_executor);
    (void)listener.Bind("127.0.0.1", 0);
    (void)listener.Listen();
    auto port = listener.GetPort();

    // Server thread: run echo loop
    std::unique_ptr<IoUringTcpStream> server_stream;
    std::atomic<bool> server_ready{false};
    std::atomic<bool> bench_done{false};

    std::thread server_thread([&]() {
        SetThreadAffinity(CpuAffinity::SingleCore(server_cpu));
        SetThreadName("bench-server");

        // Accept connection
        auto accept_task = [&]() -> Task<void> {
            server_stream = co_await listener.Accept();
            server_ready.store(true);
            server_executor.Stop();
        };
        RunTask(server_executor, accept_task());

        // Echo loop: read and write back until benchmark ends
        auto echo_task = [&]() -> Task<void> {
            while (!bench_done.load()) {
                auto data = co_await server_stream->Read(msg_size);
                if (data.empty()) break;
                co_await server_stream->Write(std::string_view(data.data(), data.size()));
            }
            server_executor.Stop();
        };
        RunTask(server_executor, echo_task());
    });

    SetThreadAffinity(CpuAffinity::SingleCore(client_cpu));

    // Client: connect
    IoUringTcpStream client_stream(client_executor);
    auto connect_task = [&]() -> Task<void> {
        co_await client_stream.Connect("127.0.0.1", port);
        // Wait for server to be ready
        while (!server_ready.load()) {
            co_await AsyncSleep(1ms);
        }
        client_executor.Stop();
    };
    RunTask(client_executor, connect_task());

    // Benchmark loop
    for (auto _ : state) {
        auto bench_task = [&]() -> Task<void> {
            co_await client_stream.Write(send_data);
            auto echo = co_await client_stream.Read(msg_size);
            benchmark::DoNotOptimize(echo);
            client_executor.Stop();
        };
        RunTask(client_executor, bench_task());
    }

    // Cleanup
    bench_done.store(true);
    client_stream.Close();

    // Wake up server if it's blocked on Read
    server_executor.Post([&]() { server_executor.Stop(); });
    server_thread.join();

    // Reset affinity
    SetThreadAffinity(CpuAffinity::None());

    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(msg_size) * 2);
}
BENCHMARK(BM_IoUringTcpRoundTrip_CrossCore)->Arg(64)->Arg(1024)->Arg(4096);

// ============================================================================
// CrossCore TCP Throughput: client sends, server reads on different cores
// ============================================================================

static void BM_IoUringTcpThroughput_CrossCore(benchmark::State& state) {
    const auto block_size = static_cast<size_t>(state.range(0));
    std::string send_data(block_size, 'U');

    int num_cpus = GetNumCpus();
    int client_cpu = 0;
    int server_cpu = (num_cpus > 1) ? 1 : 0;

    IoUringExecutor::Config config;
    config.provided_buffers = true;
    config.buffer_ring_size = 4096;

    auto client_res = IoUringExecutor::Create(config);
    auto& client_executor = *client_res.Value();

    auto server_res = IoUringExecutor::Create(config);
    auto& server_executor = *server_res.Value();

    IoUringTcpListener listener(server_executor);
    (void)listener.Bind("127.0.0.1", 0);
    (void)listener.Listen();
    auto port = listener.GetPort();

    std::unique_ptr<IoUringTcpStream> server_stream;
    std::atomic<bool> server_ready{false};
    std::atomic<bool> bench_done{false};

    // Server thread: accept then read loop
    std::thread server_thread([&]() {
        SetThreadAffinity(CpuAffinity::SingleCore(server_cpu));
        SetThreadName("bench-server");

        auto accept_task = [&]() -> Task<void> {
            server_stream = co_await listener.Accept();
            server_ready.store(true);
            server_executor.Stop();
        };
        RunTask(server_executor, accept_task());

        // Read loop: consume everything the client sends
        auto read_task = [&]() -> Task<void> {
            while (!bench_done.load()) {
                auto data = co_await server_stream->ReadProvided();
                if (data.empty()) break;
                benchmark::DoNotOptimize(data);
            }
            server_executor.Stop();
        };
        RunTask(server_executor, read_task());
    });

    SetThreadAffinity(CpuAffinity::SingleCore(client_cpu));

    // Client: connect
    IoUringTcpStream client_stream(client_executor);
    auto connect_task = [&]() -> Task<void> {
        co_await client_stream.Connect("127.0.0.1", port);
        while (!server_ready.load()) {
            co_await AsyncSleep(1ms);
        }
        client_executor.Stop();
    };
    RunTask(client_executor, connect_task());

    // Benchmark: client sends
    for (auto _ : state) {
        auto bench_task = [&]() -> Task<void> {
            auto result = co_await client_stream.Write(send_data);
            benchmark::DoNotOptimize(result);
            client_executor.Stop();
        };
        RunTask(client_executor, bench_task());
    }

    // Cleanup
    bench_done.store(true);
    client_stream.Close();
    server_executor.Post([&]() { server_executor.Stop(); });
    server_thread.join();

    SetThreadAffinity(CpuAffinity::None());

    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(block_size));
}
BENCHMARK(BM_IoUringTcpThroughput_CrossCore)->Arg(4096)->Arg(65536);

// ============================================================================
// Sync TCP Baselines
// ============================================================================

// --- Sync Round-Trip ---
static void BM_SyncTcpRoundTrip(benchmark::State& state) {
    const auto msg_size = static_cast<size_t>(state.range(0));
    std::string send_data(msg_size, 'S');

    // Set up listener
    auto listener_res = SyncTcpListener::Listen("127.0.0.1", 0);
    auto& listener = listener_res.Value();
    auto port = listener->GetPort();

    // Server thread
    std::unique_ptr<SyncTcpStream> server_conn;
    std::atomic<bool> server_ready{false};
    std::atomic<bool> bench_done{false};

    std::thread server_thread([&]() {
        auto accept_res = listener->Accept();
        server_conn = std::move(accept_res).Value();
        server_ready.store(true);

        // Echo loop
        while (!bench_done.load()) {
            auto recv_res = server_conn->RecvExact(msg_size);
            if (recv_res.IsErr()) break;
            auto send_res = server_conn->SendAll(recv_res.Value());
            if (send_res.IsErr()) break;
        }
    });

    // Client: connect after listener is set up (server accept is blocking)
    auto client_res = SyncTcpStream::Connect("127.0.0.1", port);
    auto& client = client_res.Value();

    // Wait for server to finish accept
    while (!server_ready.load()) {
        std::this_thread::sleep_for(1ms);
    }

    // Benchmark
    for (auto _ : state) {
        (void)client->SendAll(send_data);
        auto recv_res = client->RecvExact(msg_size);
        benchmark::DoNotOptimize(recv_res);
    }

    bench_done.store(true);
    client->Close();
    server_thread.join();

    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(msg_size) * 2);
}
BENCHMARK(BM_SyncTcpRoundTrip)->Arg(64)->Arg(1024)->Arg(4096);

// --- Sync Throughput ---
static void BM_SyncTcpThroughput(benchmark::State& state) {
    const auto block_size = static_cast<size_t>(state.range(0));
    std::string send_data(block_size, 'S');

    auto listener_res = SyncTcpListener::Listen("127.0.0.1", 0);
    auto& listener = listener_res.Value();
    auto port = listener->GetPort();

    std::unique_ptr<SyncTcpStream> server_conn;
    std::atomic<bool> server_ready{false};
    std::atomic<bool> bench_done{false};

    std::thread server_thread([&]() {
        auto accept_res = listener->Accept();
        server_conn = std::move(accept_res).Value();
        server_ready.store(true);

        // Read loop
        while (!bench_done.load()) {
            auto recv_res = server_conn->Recv(block_size);
            if (recv_res.IsErr() || recv_res.Value().empty()) break;
            benchmark::DoNotOptimize(recv_res);
        }
    });

    auto client_res = SyncTcpStream::Connect("127.0.0.1", port);
    auto& client = client_res.Value();

    while (!server_ready.load()) {
        std::this_thread::sleep_for(1ms);
    }

    for (auto _ : state) {
        auto send_res = client->SendAll(send_data);
        benchmark::DoNotOptimize(send_res);
    }

    bench_done.store(true);
    client->Close();
    server_thread.join();

    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(block_size));
}
BENCHMARK(BM_SyncTcpThroughput)->Arg(4096)->Arg(65536);

// ============================================================================
// Multi-Connection Benchmarks
// ============================================================================
//
// These benchmarks demonstrate io_uring's true advantage: a single thread
// multiplexing N concurrent connections vs the sync model requiring N threads.
//
// io_uring:  1 executor thread handling N echo round-trips concurrently
//            via WhenAll(). No context switches between connections.
//
// sync:      N threads, each handling 1 echo round-trip synchronously.
//            Thread creation overhead + context-switch cost at scale.
//

// --- Helper coroutine functions for multi-connection benchmarks ---
// Using free functions instead of coroutine lambdas to avoid closure lifetime
// issues: a coroutine lambda's frame holds a pointer to the temporary closure
// object, which gets destroyed while the frame is still alive.

Task<void> AcceptOne(IoUringTcpListener& listener, std::vector<std::unique_ptr<IoUringTcpStream>>& out) {
    auto stream = co_await listener.Accept();
    out.push_back(std::move(stream));
}

Task<void> ConnectOne(IoUringTcpStream& stream, std::string_view host, uint16_t port, IoUringExecutor& executor) {
    co_await AsyncSleep(5ms);
    co_await stream.Connect(std::string(host), port);
    co_await AsyncSleep(10ms);
    executor.Stop();
}

Task<void> EchoRoundTrip(IoUringTcpStream& client, IoUringTcpStream& server, std::string_view send_data,
                         size_t msg_size) {
    co_await client.Write(send_data);
    auto data = co_await server.Read(msg_size);
    benchmark::DoNotOptimize(data);
    co_await server.Write(std::string_view(data.data(), data.size()));
    auto echo = co_await client.Read(msg_size);
    benchmark::DoNotOptimize(echo);
}

Task<void> RunMultiConnBench(IoUringExecutor& executor, std::vector<std::unique_ptr<IoUringTcpStream>>& clients,
                             std::vector<std::unique_ptr<IoUringTcpStream>>& servers, std::string_view send_data,
                             size_t msg_size, size_t num_conns) {
    std::vector<Task<void>> echo_tasks;
    echo_tasks.reserve(num_conns);
    for (size_t i = 0; i < num_conns; ++i) {
        echo_tasks.push_back(EchoRoundTrip(*clients[i], *servers[i], send_data, msg_size));
    }
    co_await WhenAll(std::move(echo_tasks));
    executor.Stop();
}

// --- io_uring Multi-Connection Echo Round-Trip ---
void BM_IoUringTcpMultiConnRoundTrip(benchmark::State& state) {
    const auto num_conns = static_cast<size_t>(state.range(0));
    const size_t msg_size = 64;
    std::string send_data(msg_size, 'M');

    auto res = IoUringExecutor::Create();
    auto& executor = *res.Value();

    // Set up listener
    IoUringTcpListener listener(executor);
    (void)listener.Bind("127.0.0.1", 0);
    (void)listener.Listen();
    auto port = listener.GetPort();

    // Establish N connections
    std::vector<std::unique_ptr<IoUringTcpStream>> server_streams;
    std::vector<std::unique_ptr<IoUringTcpStream>> client_streams;

    server_streams.reserve(num_conns);
    client_streams.reserve(num_conns);

    for (size_t i = 0; i < num_conns; ++i) {
        client_streams.push_back(std::make_unique<IoUringTcpStream>(executor));
    }

    // Establish N connections one at a time
    for (size_t i = 0; i < num_conns; ++i) {
        auto at = AcceptOne(listener, server_streams);
        auto ct = ConnectOne(*client_streams[i], "127.0.0.1", port, executor);
        executor.Schedule(at.GetHandle());
        executor.Schedule(ct.GetHandle());
        executor.Run();
    }

    // Benchmark: N concurrent echo round-trips using WhenAll
    for (auto _ : state) {
        auto task = RunMultiConnBench(executor, client_streams, server_streams, send_data, msg_size, num_conns);
        RunTask(executor, std::move(task));
    }

    // Cleanup
    for (auto& s : client_streams) {
        if (s) s->Close();
    }
    for (auto& s : server_streams) {
        if (s) s->Close();
    }

    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(num_conns) *
                            static_cast<int64_t>(msg_size) * 2);
    state.counters["conns"] = static_cast<double>(num_conns);
}
BENCHMARK(BM_IoUringTcpMultiConnRoundTrip)->Arg(4)->Arg(16)->Arg(64)->Arg(256);

// --- Sync Multi-Connection Echo Round-Trip ---
// Each connection gets its own pair of threads (server + client).
// Measures aggregate echo round-trip time across N connections.
static void BM_SyncTcpMultiConnRoundTrip(benchmark::State& state) {
    const auto num_conns = static_cast<size_t>(state.range(0));
    const size_t msg_size = 64;
    std::string send_data(msg_size, 'M');

    // Set up listener
    auto listener_res = SyncTcpListener::Listen("127.0.0.1", 0);
    auto& listener = listener_res.Value();
    auto port = listener->GetPort();

    // Establish N connections
    std::vector<std::unique_ptr<SyncTcpStream>> server_conns(num_conns);
    std::vector<std::unique_ptr<SyncTcpStream>> client_conns(num_conns);

    // Accept thread
    std::thread accept_thread([&]() {
        for (size_t i = 0; i < num_conns; ++i) {
            auto accept_res = listener->Accept();
            server_conns[i] = std::move(accept_res).Value();
        }
    });

    // Connect all clients
    for (size_t i = 0; i < num_conns; ++i) {
        auto conn_res = SyncTcpStream::Connect("127.0.0.1", port);
        client_conns[i] = std::move(conn_res).Value();
    }
    accept_thread.join();

    // Server echo threads: one per connection
    std::atomic<bool> bench_done{false};
    std::vector<std::thread> echo_threads;
    echo_threads.reserve(num_conns);

    for (size_t i = 0; i < num_conns; ++i) {
        echo_threads.emplace_back([&, i]() {
            while (!bench_done.load(std::memory_order_relaxed)) {
                auto recv_res = server_conns[i]->RecvExact(msg_size);
                if (recv_res.IsErr()) break;
                auto send_res = server_conns[i]->SendAll(recv_res.Value());
                if (send_res.IsErr()) break;
            }
        });
    }

    // Benchmark: N client threads doing echo round-trips concurrently
    std::atomic<bool> running{true};
    std::barrier start_barrier(static_cast<std::ptrdiff_t>(num_conns + 1));
    std::barrier done_barrier(static_cast<std::ptrdiff_t>(num_conns + 1));

    std::vector<std::thread> client_threads;
    client_threads.reserve(num_conns);

    for (size_t i = 0; i < num_conns; ++i) {
        client_threads.emplace_back([&, i]() {
            while (true) {
                start_barrier.arrive_and_wait();
                if (!running.load(std::memory_order_relaxed)) {
                    return;
                }
                auto send_res = client_conns[i]->SendAll(send_data);
                if (!send_res.IsErr()) {
                    auto recv_res = client_conns[i]->RecvExact(msg_size);
                    benchmark::DoNotOptimize(recv_res);
                }
                done_barrier.arrive_and_wait();
            }
        });
    }

    for (auto _ : state) {
        start_barrier.arrive_and_wait();
        done_barrier.arrive_and_wait();
    }

    // Cleanup
    running.store(false, std::memory_order_relaxed);
    start_barrier.arrive_and_wait();

    for (auto& t : client_threads) {
        t.join();
    }

    bench_done.store(true, std::memory_order_relaxed);
    for (auto& c : client_conns) {
        if (c) c->Close();
    }
    for (auto& t : echo_threads) {
        t.join();
    }

    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(num_conns) *
                            static_cast<int64_t>(msg_size) * 2);
    state.counters["conns"] = static_cast<double>(num_conns);
}
BENCHMARK(BM_SyncTcpMultiConnRoundTrip)->Arg(4)->Arg(16)->Arg(64)->Arg(256);

}  // namespace

#endif  // HOTCOCO_HAS_IOURING
