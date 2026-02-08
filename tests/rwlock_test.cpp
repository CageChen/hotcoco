// ============================================================================
// RWLock Tests
// ============================================================================

#include "hotcoco/sync/rwlock.hpp"

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
// Basic RWLock Tests
// ============================================================================

TEST(RWLockTest, ReadLock) {
    AsyncRWLock lock;

    auto test = [&lock]() -> Task<void> {
        auto guard = co_await lock.ReadLock();
        // Can read
    };

    SyncWait(test());
    SUCCEED();
}

TEST(RWLockTest, WriteLock) {
    AsyncRWLock lock;

    auto test = [&lock]() -> Task<void> {
        auto guard = co_await lock.WriteLock();
        // Exclusive access
    };

    SyncWait(test());
    SUCCEED();
}

TEST(RWLockTest, MultipleReaders) {
    AsyncRWLock lock;

    auto test = [&lock]() -> Task<void> {
        auto g1 = co_await lock.ReadLock();
        // In real usage, multiple readers would acquire simultaneously
        // This test just verifies the API works
    };

    SyncWait(test());
    SUCCEED();
}

// ============================================================================
// Concurrent RWLock Tests
// ============================================================================

Task<void> WriterTask(AsyncRWLock& lock, int& data, std::atomic<int>& completed) {
    auto guard = co_await lock.WriteLock();
    data++;
    completed++;
}

Task<void> ReaderTask(AsyncRWLock& lock, int& data, std::atomic<int>& completed) {
    auto guard = co_await lock.ReadLock();
    (void)data;  // Read data
    completed++;
}

TEST(RWLockTest, ProtectsData) {
    AsyncRWLock lock;
    int data = 0;
    std::atomic<int> completed{0};
    ThreadPoolExecutor executor(4);
    constexpr int kWriters = 10;
    constexpr int kReaders = 20;

    // Writers
    for (int i = 0; i < kWriters; i++) {
        Spawn(executor, WriterTask(lock, data, completed));
    }

    // Readers
    for (int i = 0; i < kReaders; i++) {
        Spawn(executor, ReaderTask(lock, data, completed));
    }

    // Wait for completion
    auto start = std::chrono::steady_clock::now();
    while (completed.load() < kWriters + kReaders) {
        std::this_thread::sleep_for(20ms);
        if (std::chrono::steady_clock::now() - start > 5s) {
            FAIL() << "Timeout: " << completed.load();
        }
    }

    executor.Stop();
    EXPECT_EQ(data, kWriters);
}

// ============================================================================
// Additional Edge Cases
// ============================================================================

TEST(RWLockTest, ReadThenWrite) {
    AsyncRWLock lock;

    auto test = [&]() -> Task<void> {
        {
            auto rg = co_await lock.ReadLock();
        }
        {
            auto wg = co_await lock.WriteLock();
        }
    };

    SyncWait(test());
}

TEST(RWLockTest, WriteThenRead) {
    AsyncRWLock lock;

    auto test = [&]() -> Task<void> {
        {
            auto wg = co_await lock.WriteLock();
        }
        {
            auto rg = co_await lock.ReadLock();
        }
    };

    SyncWait(test());
}

TEST(RWLockTest, ReadGuardMove) {
    AsyncRWLock lock;

    auto test = [&]() -> Task<void> {
        auto rg1 = co_await lock.ReadLock();
        auto rg2 = std::move(rg1);
        // rg2 goes out of scope and unlocks
    };

    SyncWait(test());
}

TEST(RWLockTest, WriteGuardMove) {
    AsyncRWLock lock;

    auto test = [&]() -> Task<void> {
        auto wg1 = co_await lock.WriteLock();
        auto wg2 = std::move(wg1);
    };

    SyncWait(test());

    // Verify write lock is released
    auto test2 = [&]() -> Task<void> { auto rg = co_await lock.ReadLock(); };
    SyncWait(test2());
}
