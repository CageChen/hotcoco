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
