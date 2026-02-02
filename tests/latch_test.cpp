// ============================================================================
// Latch Tests
// ============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "hotcoco/core/spawn.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/io/thread_pool_executor.hpp"
#include "hotcoco/sync/latch.hpp"
#include "hotcoco/sync/sync_wait.hpp"

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// Basic Latch Tests
// ============================================================================

TEST(LatchTest, InitialCount) {
    AsyncLatch latch(5);
    EXPECT_EQ(latch.Count(), 5);
    EXPECT_FALSE(latch.IsReleased());
}

TEST(LatchTest, CountDownToZero) {
    AsyncLatch latch(3);
    latch.CountDown();
    EXPECT_EQ(latch.Count(), 2);
    latch.CountDown();
    EXPECT_EQ(latch.Count(), 1);
    latch.CountDown();
    EXPECT_EQ(latch.Count(), 0);
    EXPECT_TRUE(latch.IsReleased());
}

TEST(LatchTest, WaitWhenAlreadyZero) {
    AsyncLatch latch(0);
    EXPECT_TRUE(latch.IsReleased());
    
    auto test = [&latch]() -> Task<void> {
        co_await latch.Wait();  // Should return immediately
    };
    
    SyncWait(test());
    SUCCEED();
}

TEST(LatchTest, CountDownByN) {
    AsyncLatch latch(10);
    latch.CountDown(5);
    EXPECT_EQ(latch.Count(), 5);
    latch.CountDown(5);
    EXPECT_TRUE(latch.IsReleased());
}

// ============================================================================
// Concurrent Latch Tests
// ============================================================================

Task<void> WaitAndIncrement(AsyncLatch& latch, std::atomic<int>& woken) {
    co_await latch.Wait();
    woken++;
}

TEST(LatchTest, MultipleWaiters) {
    AsyncLatch latch(3);
    std::atomic<int> woken{0};
    ThreadPoolExecutor executor(4);

    // Start multiple waiters
    for (int i = 0; i < 5; i++) {
        Spawn(executor, WaitAndIncrement(latch, woken));
    }
    
    // Give waiters time to start
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(woken.load(), 0);
    
    // Count down
    latch.CountDown();
    latch.CountDown();
    latch.CountDown();
    
    // Wait for wakeup
    auto start = std::chrono::steady_clock::now();
    while (woken.load() < 5) {
        std::this_thread::sleep_for(10ms);
        if (std::chrono::steady_clock::now() - start > 2s) {
            FAIL() << "Timeout: woken=" << woken.load();
        }
    }
    
    executor.Stop();
    EXPECT_EQ(woken.load(), 5);
}

Task<void> WaitAndSetFlag(AsyncLatch& latch, std::atomic<bool>& completed) {
    co_await latch.Wait();  // Should not block
    completed = true;
}

TEST(LatchTest, WaiterAfterRelease) {
    AsyncLatch latch(1);
    std::atomic<bool> completed{false};
    ThreadPoolExecutor executor(2);

    // Release latch
    latch.CountDown();
    EXPECT_TRUE(latch.IsReleased());

    // Waiter should complete immediately
    Spawn(executor, WaitAndSetFlag(latch, completed));
    
    auto start = std::chrono::steady_clock::now();
    while (!completed.load()) {
        std::this_thread::sleep_for(10ms);
        if (std::chrono::steady_clock::now() - start > 1s) {
            FAIL() << "Timeout waiting for post-release waiter";
        }
    }
    
    executor.Stop();
    EXPECT_TRUE(completed.load());
}

// ============================================================================
// Additional Edge Cases
// ============================================================================

TEST(LatchTest, CountDownPastZero) {
    AsyncLatch latch(2);
    latch.CountDown(5);  // Should clamp to 0, not underflow
    EXPECT_TRUE(latch.IsReleased());
    EXPECT_EQ(latch.Count(), 0u);
}

TEST(LatchTest, CountDownByZero) {
    AsyncLatch latch(3);
    latch.CountDown(0);
    EXPECT_EQ(latch.Count(), 3u);
    EXPECT_FALSE(latch.IsReleased());
}

TEST(LatchTest, SingleCountDown) {
    AsyncLatch latch(1);
    EXPECT_FALSE(latch.IsReleased());
    latch.CountDown();
    EXPECT_TRUE(latch.IsReleased());
}
