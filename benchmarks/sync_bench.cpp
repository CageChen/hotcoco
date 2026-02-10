// ============================================================================
// Synchronization Primitives Benchmarks
// ============================================================================
//
// Baseline results (2026-02-08, GCC Release, AMD Ryzen 9 9950X 16C/32T @ 6.0GHz):
//
//   Benchmark                              Time        CPU      Iterations
//   ----------------------------------------------------------------------
//   BM_MutexUncontended                   27.4 ns     27.4 ns    25643498
//   BM_MutexUncontendedAmortized          5678 ns     5677 ns      122963  176M items/s
//   BM_StdMutexUncontended                2.64 ns     2.64 ns   256230428
//   BM_SemaphoreUncontended               30.3 ns     30.3 ns    23203750
//   BM_ChannelSendReceive                 30.1 ns     30.1 ns    23208199
//   BM_ChannelBufferedThroughput/1        31.8 ns     31.8 ns    21983229   63M items/s
//   BM_ChannelBufferedThroughput/8         104 ns      104 ns     6785733  154M items/s
//   BM_ChannelBufferedThroughput/64        578 ns      578 ns     1212496  222M items/s
//   BM_ChannelBufferedThroughput/512      4332 ns     4332 ns      161740  236M items/s
//   BM_ChannelBufferedThroughput/1024     8641 ns     8640 ns       81181  237M items/s
//   BM_ThreadPoolSchedule                75331 ns    63044 ns       12621
//   BM_LatchCountDown                     1112 ns     1112 ns      629124
//   BM_RWLockReadUncontended              45.7 ns     45.7 ns    15313247
//   BM_RWLockWriteUncontended             46.0 ns     46.0 ns    15305167
//   BM_StdSharedMutexRead                 7.89 ns     7.89 ns    88993107
//   BM_StdSharedMutexWrite                12.0 ns     12.0 ns    58760206
//
// ============================================================================

#include "hotcoco/core/channel.hpp"
#include "hotcoco/core/spawn.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/io/thread_pool_executor.hpp"
#include "hotcoco/sync/latch.hpp"
#include "hotcoco/sync/mutex.hpp"
#include "hotcoco/sync/rwlock.hpp"
#include "hotcoco/sync/semaphore.hpp"
#include "hotcoco/sync/sync_wait.hpp"

#include <benchmark/benchmark.h>
#include <shared_mutex>
#include <thread>
#include <vector>

using namespace hotcoco;

// ============================================================================
// Mutex Benchmarks
// ============================================================================

// Uncontended mutex lock/unlock using guard
static void BM_MutexUncontended(benchmark::State& state) {
    AsyncMutex mutex;

    for (auto _ : state) {
        SyncWait([&mutex]() -> Task<void> {
            auto lock = co_await mutex.Lock();
            // Critical section - lock automatically released
            co_return;
        }());
    }
}
BENCHMARK(BM_MutexUncontended);

// Amortized lock/unlock cost: many iterations inside one SyncWait to remove
// coroutine creation overhead from the measurement.
static void BM_MutexUncontendedAmortized(benchmark::State& state) {
    AsyncMutex mutex;
    const int batch = 1000;

    for (auto _ : state) {
        SyncWait([&mutex, batch]() -> Task<void> {
            for (int i = 0; i < batch; ++i) {
                auto lock = co_await mutex.Lock();
            }
            co_return;
        }());
    }
    state.SetItemsProcessed(state.iterations() * batch);
}
BENCHMARK(BM_MutexUncontendedAmortized);

// std::mutex for comparison
static void BM_StdMutexUncontended(benchmark::State& state) {
    std::mutex mutex;

    for (auto _ : state) {
        std::lock_guard<std::mutex> lock(mutex);
        // Critical section
    }
}
BENCHMARK(BM_StdMutexUncontended);

// ============================================================================
// Semaphore Benchmarks
// ============================================================================

static void BM_SemaphoreUncontended(benchmark::State& state) {
    AsyncSemaphore sem(1);

    for (auto _ : state) {
        SyncWait([&sem]() -> Task<void> {
            co_await sem.Acquire();
            sem.Release();
            co_return;
        }());
    }
}
BENCHMARK(BM_SemaphoreUncontended);

// ============================================================================
// Channel Benchmarks
// ============================================================================

static void BM_ChannelSendReceive(benchmark::State& state) {
    Channel<int> ch(1);

    for (auto _ : state) {
        SyncWait([&ch]() -> Task<void> {
            co_await ch.Send(42);
            auto val = co_await ch.Receive();
            benchmark::DoNotOptimize(val);
            co_return;
        }());
    }
}
BENCHMARK(BM_ChannelSendReceive);

// Buffered channel throughput
static void BM_ChannelBufferedThroughput(benchmark::State& state) {
    const int buffer_size = state.range(0);
    Channel<int> ch(buffer_size);

    for (auto _ : state) {
        SyncWait([&ch, buffer_size]() -> Task<void> {
            // Fill buffer
            for (int i = 0; i < buffer_size; ++i) {
                co_await ch.Send(i);
            }
            // Drain buffer
            for (int i = 0; i < buffer_size; ++i) {
                auto val = co_await ch.Receive();
                benchmark::DoNotOptimize(val);
            }
            co_return;
        }());
    }
    state.SetItemsProcessed(state.iterations() * buffer_size * 2);
}
BENCHMARK(BM_ChannelBufferedThroughput)->Range(1, 1024);

// ============================================================================
// ThreadPoolExecutor Benchmarks
// ============================================================================

static void BM_ThreadPoolSchedule(benchmark::State& state) {
    ThreadPoolExecutor executor(4);
    std::atomic<int> counter{0};

    for (auto _ : state) {
        counter = 0;
        std::atomic<int> completed{0};
        const int num_tasks = 100;

        for (int i = 0; i < num_tasks; ++i) {
            Spawn(executor, [&counter, &completed]() -> Task<void> {
                counter++;
                completed++;
                co_return;
            }());
        }

        while (completed.load() < num_tasks) {
            std::this_thread::yield();
        }
    }

    executor.Stop();
}
BENCHMARK(BM_ThreadPoolSchedule);

// ============================================================================
// Latch Benchmarks
// ============================================================================

static void BM_LatchCountDown(benchmark::State& state) {
    for (auto _ : state) {
        AsyncLatch latch(100);
        for (int i = 0; i < 100; ++i) {
            latch.CountDown();
        }
    }
}
BENCHMARK(BM_LatchCountDown);

// ============================================================================
// RWLock Benchmarks
// ============================================================================

// Uncontended read lock
static void BM_RWLockReadUncontended(benchmark::State& state) {
    AsyncRWLock rwlock;

    for (auto _ : state) {
        SyncWait([&rwlock]() -> Task<void> {
            auto guard = co_await rwlock.ReadLock();
            co_return;
        }());
    }
}
BENCHMARK(BM_RWLockReadUncontended);

// Uncontended write lock
static void BM_RWLockWriteUncontended(benchmark::State& state) {
    AsyncRWLock rwlock;

    for (auto _ : state) {
        SyncWait([&rwlock]() -> Task<void> {
            auto guard = co_await rwlock.WriteLock();
            co_return;
        }());
    }
}
BENCHMARK(BM_RWLockWriteUncontended);

// std::shared_mutex for comparison
static void BM_StdSharedMutexRead(benchmark::State& state) {
    std::shared_mutex mutex;

    for (auto _ : state) {
        std::shared_lock<std::shared_mutex> lock(mutex);
    }
}
BENCHMARK(BM_StdSharedMutexRead);

static void BM_StdSharedMutexWrite(benchmark::State& state) {
    std::shared_mutex mutex;

    for (auto _ : state) {
        std::unique_lock<std::shared_mutex> lock(mutex);
    }
}
BENCHMARK(BM_StdSharedMutexWrite);
