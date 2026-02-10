// ============================================================================
// Mutex Tests
// ============================================================================

#include "hotcoco/sync/mutex.hpp"

#include "hotcoco/core/spawn.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/io/thread_pool_executor.hpp"
#include "hotcoco/sync/sync_wait.hpp"

#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// Basic Mutex Tests
// ============================================================================

TEST(MutexTest, LockUnlock) {
    AsyncMutex mutex;

    auto test = [&mutex]() -> Task<void> {
        {
            auto lock = co_await mutex.Lock();
            // Critical section
        }
    };

    SyncWait(test());
    SUCCEED();
}

TEST(MutexTest, TryLockWhenFree) {
    AsyncMutex mutex;

    auto test = [&mutex]() -> Task<void> {
        auto lock = co_await mutex.TryLock();
        EXPECT_TRUE(lock.has_value());
    };

    SyncWait(test());
}

TEST(MutexTest, NestedLocks) {
    AsyncMutex mutex;

    auto test = [&mutex]() -> Task<void> {
        {
            auto lock1 = co_await mutex.Lock();
            // Can't nest lock same mutex (would deadlock in single-threaded)
            // This test just verifies we can release and reacquire
        }
        {
            auto lock2 = co_await mutex.Lock();
            // Second acquisition works
        }
    };

    SyncWait(test());
    SUCCEED();
}

// ============================================================================
// Concurrent Mutex Tests
// ============================================================================

Task<void> LockAndIncrement(AsyncMutex& mutex, int& counter, std::atomic<int>& completed) {
    auto lock = co_await mutex.Lock();
    counter++;
    completed++;
}

TEST(MutexTest, ProtectsCounter) {
    AsyncMutex mutex;
    int counter = 0;
    std::atomic<int> completed{0};
    ThreadPoolExecutor executor(4);
    constexpr int kIterations = 100;

    for (int i = 0; i < kIterations; i++) {
        Spawn(executor, LockAndIncrement(mutex, counter, completed));
    }

    // Wait for completion
    auto start = std::chrono::steady_clock::now();
    while (completed.load() < kIterations) {
        std::this_thread::sleep_for(10ms);
        if (std::chrono::steady_clock::now() - start > 5s) {
            FAIL() << "Timeout: " << completed.load();
        }
    }

    executor.Stop();
    EXPECT_EQ(counter, kIterations);
}

// ============================================================================
// Additional Edge Cases
// ============================================================================

TEST(MutexTest, TryLockWhenHeld) {
    AsyncMutex mutex;

    auto test = [&]() -> Task<void> {
        auto lock = co_await mutex.Lock();

        // TryLock while held should return nullopt
        auto try_result = co_await mutex.TryLock();
        EXPECT_FALSE(try_result.has_value());
    };

    SyncWait(test());
}

TEST(MutexTest, ScopedLockMove) {
    AsyncMutex mutex;

    auto test = [&]() -> Task<void> {
        auto lock1 = co_await mutex.Lock();
        // Move lock1 into lock2
        auto lock2 = std::move(lock1);
        // lock2 goes out of scope and unlocks
    };

    SyncWait(test());

    // Should be unlocked now — try lock again
    auto test2 = [&]() -> Task<void> {
        auto lock = co_await mutex.TryLock();
        EXPECT_TRUE(lock.has_value());
    };

    SyncWait(test2());
}

TEST(MutexTest, ReacquireAfterRelease) {
    AsyncMutex mutex;

    auto test = [&]() -> Task<void> {
        for (int i = 0; i < 10; ++i) {
            auto lock = co_await mutex.Lock();
            // Critical section
        }
    };

    SyncWait(test());
}

TEST(MutexTest, SequentialLockUnlock) {
    AsyncMutex mutex;
    int counter = 0;

    auto test = [&]() -> Task<void> {
        for (int i = 0; i < 100; ++i) {
            auto lock = co_await mutex.Lock();
            counter++;
        }
    };

    SyncWait(test());
    EXPECT_EQ(counter, 100);
}

// ============================================================================
// Stress Tests
// ============================================================================

// Free-function coroutines to avoid lambda capture issues under ASan
Task<void> StressLockAndIncrement(AsyncMutex& mutex, int& counter, std::atomic<int>& completed) {
    auto lock = co_await mutex.Lock();
    counter++;
    completed++;
}

Task<void> StressTryLock(AsyncMutex& mutex, std::atomic<int>& successes, std::atomic<int>& completed) {
    auto lock = co_await mutex.TryLock();
    if (lock.has_value()) {
        successes++;
    }
    completed++;
}

Task<void> StressTryLockWithCounter(AsyncMutex& mutex, int& counter, std::atomic<int>& completed) {
    auto lock = co_await mutex.TryLock();
    if (lock.has_value()) {
        counter++;
    }
    completed++;
}

TEST(MutexTest, FastPathUncontended) {
    AsyncMutex mutex;
    int counter = 0;

    auto test = [&]() -> Task<void> {
        for (int i = 0; i < 10000; ++i) {
            auto lock = co_await mutex.Lock();
            counter++;
        }
    };

    SyncWait(test());
    EXPECT_EQ(counter, 10000);
}

TEST(MutexTest, HighContentionStress) {
    AsyncMutex mutex;
    int counter = 0;
    std::atomic<int> completed{0};
    constexpr int kThreads = 8;
    constexpr int kTasksPerThread = 1000;
    ThreadPoolExecutor executor(kThreads);

    for (int i = 0; i < kThreads * kTasksPerThread; i++) {
        Spawn(executor, StressLockAndIncrement(mutex, counter, completed));
    }

    auto start = std::chrono::steady_clock::now();
    while (completed.load() < kThreads * kTasksPerThread) {
        std::this_thread::sleep_for(1ms);
        if (std::chrono::steady_clock::now() - start > 30s) {
            FAIL() << "Timeout: " << completed.load() << " / " << kThreads * kTasksPerThread;
        }
    }

    executor.Stop();
    EXPECT_EQ(counter, kThreads * kTasksPerThread);
}

TEST(MutexTest, ConcurrentTryLockStress) {
    AsyncMutex mutex;
    constexpr int kThreads = 8;
    constexpr int kIterations = 10000;
    std::atomic<int> successes{0};
    std::atomic<int> completed{0};
    ThreadPoolExecutor executor(kThreads);

    for (int i = 0; i < kIterations; i++) {
        Spawn(executor, StressTryLock(mutex, successes, completed));
    }

    auto start = std::chrono::steady_clock::now();
    while (completed.load() < kIterations) {
        std::this_thread::sleep_for(1ms);
        if (std::chrono::steady_clock::now() - start > 10s) {
            FAIL() << "Timeout: " << completed.load();
        }
    }

    executor.Stop();
    EXPECT_GT(successes.load(), 0);
}

TEST(MutexTest, MixedLockAndTryLock) {
    AsyncMutex mutex;
    int counter = 0;
    std::atomic<int> completed{0};
    constexpr int kIterations = 1000;
    ThreadPoolExecutor executor(4);

    for (int i = 0; i < kIterations; i++) {
        if (i % 2 == 0) {
            Spawn(executor, StressLockAndIncrement(mutex, counter, completed));
        } else {
            Spawn(executor, StressTryLockWithCounter(mutex, counter, completed));
        }
    }

    auto start = std::chrono::steady_clock::now();
    while (completed.load() < kIterations) {
        std::this_thread::sleep_for(1ms);
        if (std::chrono::steady_clock::now() - start > 10s) {
            FAIL() << "Timeout: " << completed.load();
        }
    }

    executor.Stop();
    // At minimum, all Lock() calls succeeded (kIterations/2)
    EXPECT_GE(counter, kIterations / 2);
}

Task<void> FIFOWaiter(AsyncMutex& mutex, std::vector<int>& order, int id) {
    auto lock = co_await mutex.Lock();
    order.push_back(id);
}

TEST(MutexTest, WaiterFIFOOrder) {
    AsyncMutex mutex;
    std::vector<int> order;

    auto fifo_test = [&]() -> Task<void> {
        auto task1 = FIFOWaiter(mutex, order, 1);
        auto task2 = FIFOWaiter(mutex, order, 2);
        auto task3 = FIFOWaiter(mutex, order, 3);

        {
            auto outer_lock = co_await mutex.Lock();

            // Resume tasks — each starts, hits Lock(), and suspends as a waiter
            task1.GetHandle().resume();
            task2.GetHandle().resume();
            task3.GetHandle().resume();

            // outer_lock released here → wakes waiters in FIFO chain
        }

        // All three tasks have completed by now (chain reaction from Unlock)
    };

    SyncWait(fifo_test());

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
}

TEST(MutexTest, RapidLockUnlockAcrossThreads) {
    AsyncMutex mutex;
    int counter = 0;
    std::atomic<int> completed{0};
    constexpr int kPerThread = 10000;
    constexpr int kThreads = 2;
    ThreadPoolExecutor executor(kThreads);

    for (int t = 0; t < kThreads; t++) {
        for (int i = 0; i < kPerThread; i++) {
            Spawn(executor, StressLockAndIncrement(mutex, counter, completed));
        }
    }

    auto start = std::chrono::steady_clock::now();
    while (completed.load() < kThreads * kPerThread) {
        std::this_thread::sleep_for(1ms);
        if (std::chrono::steady_clock::now() - start > 30s) {
            FAIL() << "Timeout: " << completed.load() << " / " << kThreads * kPerThread;
        }
    }

    executor.Stop();
    EXPECT_EQ(counter, kThreads * kPerThread);
}
