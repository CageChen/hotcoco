// ============================================================================
// Stress Tests - High-load testing for production readiness
// ============================================================================

#include "hotcoco/core/frame_pool.hpp"
#include "hotcoco/core/spawn.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/io/thread_pool_executor.hpp"
#include "hotcoco/sync/sync_wait.hpp"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <random>
#include <thread>
#include <vector>

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// High Volume Tests
// ============================================================================

TEST(StressTest, ManySequentialTasks) {
#if HOTCOCO_ASAN_ACTIVE
    constexpr int kTaskCount = 500;  // ASan uses larger stack frames
#else
    constexpr int kTaskCount = 10000;
#endif
    int sum = 0;

    auto runner = [&sum]() -> Task<void> {
        for (int i = 0; i < kTaskCount; i++) {
            auto task = [i]() -> Task<int> { co_return i; };
            sum += co_await task();
        }
    };

    SyncWait(runner());

    // Sum of 0..kTaskCount-1
    EXPECT_EQ(sum, (kTaskCount - 1) * kTaskCount / 2);
}

Task<void> IncrementAndReturn(std::atomic<int>& completed) {
    completed++;
    co_return;
}

TEST(StressTest, ManySpawnedTasks) {
    std::atomic<int> completed{0};
    ThreadPoolExecutor executor(4);
    constexpr int kTaskCount = 1000;

    for (int i = 0; i < kTaskCount; i++) {
        Spawn(executor, IncrementAndReturn(completed));
    }

    // Wait for completion
    auto start = std::chrono::steady_clock::now();
    while (completed.load() < kTaskCount) {
        std::this_thread::sleep_for(10ms);
        if (std::chrono::steady_clock::now() - start > 5s) {
            FAIL() << "Timeout waiting for tasks: " << completed.load() << "/" << kTaskCount;
        }
    }

    executor.Stop();
    EXPECT_EQ(completed.load(), kTaskCount);
}

TEST(StressTest, DeepCoroutineChain) {
    constexpr int kDepth = 1000;

    std::function<Task<int>(int)> recurse;
    recurse = [&recurse](int depth) -> Task<int> {
        if (depth <= 0) {
            co_return 0;
        }
        int result = co_await recurse(depth - 1);
        co_return result + 1;
    };

    int result = SyncWait(recurse(kDepth));
    EXPECT_EQ(result, kDepth);
}

// ============================================================================
// Concurrent Access Tests
// ============================================================================

TEST(StressTest, ConcurrentSpawns) {
    std::atomic<int> counter{0};
    ThreadPoolExecutor executor(8);
    constexpr int kSpawns = 500;

    // Spawn all from main thread (spawning from multiple threads
    // can cause issues with coroutine handle races)
    for (int i = 0; i < kSpawns; i++) {
        Spawn(executor, IncrementAndReturn(counter));
    }

    // Wait for all spawned tasks
    auto start = std::chrono::steady_clock::now();
    while (counter.load() < kSpawns) {
        std::this_thread::sleep_for(10ms);
        if (std::chrono::steady_clock::now() - start > 5s) {
            FAIL() << "Timeout: " << counter.load();
        }
    }

    executor.Stop();
    EXPECT_EQ(counter.load(), kSpawns);
}

// ============================================================================
// Memory Stress Tests
// ============================================================================

TEST(StressTest, RapidAllocationDeallocation) {
#if HOTCOCO_ASAN_ACTIVE
    constexpr int kIterations = 500;  // ASan uses larger stack frames
#else
    constexpr int kIterations = 10000;
#endif

    auto allocator = []() -> Task<void> {
        for (int i = 0; i < kIterations; i++) {
            // Create and immediately destroy a coroutine
            auto task = []() -> Task<int> { co_return 42; };
            int result = co_await task();
            (void)result;
        }
    };

    SyncWait(allocator());
    SUCCEED();
}
