// ============================================================================
// Semaphore Tests
// ============================================================================

#include "hotcoco/sync/semaphore.hpp"

#include "hotcoco/core/spawn.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/io/thread_pool_executor.hpp"
#include "hotcoco/sync/sync_wait.hpp"

#include <atomic>
#include <gtest/gtest.h>
#include <thread>

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// Basic Semaphore Tests
// ============================================================================

TEST(SemaphoreTest, AcquireRelease) {
    AsyncSemaphore sem(1);

    auto test = [&sem]() -> Task<void> {
        {
            auto guard = co_await sem.Acquire();
            EXPECT_EQ(sem.Available(), 0);
        }
        EXPECT_EQ(sem.Available(), 1);
    };

    SyncWait(test());
}

TEST(SemaphoreTest, MultipleAcquires) {
    AsyncSemaphore sem(3);

    auto test = [&sem]() -> Task<void> {
        auto g1 = co_await sem.Acquire();
        auto g2 = co_await sem.Acquire();
        auto g3 = co_await sem.Acquire();
        EXPECT_EQ(sem.Available(), 0);
    };

    SyncWait(test());
    EXPECT_EQ(sem.Available(), 3);
}

// ============================================================================
// Concurrency Tests
// ============================================================================

Task<void> AcquireAndTrack(AsyncSemaphore& sem, std::atomic<int>& concurrent, std::atomic<int>& max_concurrent,
                           std::atomic<int>& completed) {
    auto guard = co_await sem.Acquire();
    int val = ++concurrent;

    // Track max concurrency
    int cur_max = max_concurrent.load();
    while (val > cur_max && !max_concurrent.compare_exchange_weak(cur_max, val)) {
    }

    std::this_thread::sleep_for(10ms);
    concurrent--;
    completed++;
}

TEST(SemaphoreTest, LimitsConcurrency) {
    AsyncSemaphore sem(3);
    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<int> completed{0};
    ThreadPoolExecutor executor(8);
    constexpr int kTasks = 20;

    for (int i = 0; i < kTasks; i++) {
        Spawn(executor, AcquireAndTrack(sem, concurrent, max_concurrent, completed));
    }

    // Wait for completion
    auto start = std::chrono::steady_clock::now();
    while (completed.load() < kTasks) {
        std::this_thread::sleep_for(20ms);
        if (std::chrono::steady_clock::now() - start > 10s) {
            FAIL() << "Timeout: " << completed.load();
        }
    }

    executor.Stop();
    EXPECT_LE(max_concurrent.load(), 3);
    EXPECT_EQ(completed.load(), kTasks);
}

// ============================================================================
// Additional Edge Cases
// ============================================================================

TEST(SemaphoreTest, BinarySemaphore) {
    AsyncSemaphore sem(1);

    auto test = [&]() -> Task<void> {
        {
            auto g = co_await sem.Acquire();
            EXPECT_EQ(sem.Available(), 0u);
        }
        EXPECT_EQ(sem.Available(), 1u);
    };

    SyncWait(test());
}

TEST(SemaphoreTest, GuardMoveAssignment) {
    AsyncSemaphore sem(2);

    auto test = [&]() -> Task<void> {
        auto g1 = co_await sem.Acquire();
        auto g2 = co_await sem.Acquire();
        EXPECT_EQ(sem.Available(), 0u);

        // Move g2 over g1 — g1's old slot released
        g1 = std::move(g2);
        EXPECT_EQ(sem.Available(), 1u);
    };

    SyncWait(test());
    EXPECT_EQ(sem.Available(), 2u);
}

TEST(SemaphoreTest, AcquireReleaseCycle) {
    AsyncSemaphore sem(3);

    auto test = [&]() -> Task<void> {
        for (int i = 0; i < 20; ++i) {
            auto g = co_await sem.Acquire();
        }
    };

    SyncWait(test());
    EXPECT_EQ(sem.Available(), 3u);
}

TEST(SemaphoreTest, ReleasedCountMatchesAcquired) {
    AsyncSemaphore sem(5);
    std::atomic<int> completed{0};
    ThreadPoolExecutor executor(8);

    auto work = [&]() -> Task<void> {
        auto g = co_await sem.Acquire();
        std::this_thread::sleep_for(1ms);
        completed++;
    };

    for (int i = 0; i < 50; ++i) {
        Spawn(executor, work());
    }

    auto start = std::chrono::steady_clock::now();
    while (completed.load() < 50) {
        std::this_thread::sleep_for(20ms);
        if (std::chrono::steady_clock::now() - start > 10s) {
            FAIL() << "Timeout: " << completed.load();
        }
    }

    executor.Stop();
    EXPECT_EQ(sem.Available(), 5u);
}
