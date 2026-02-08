// ============================================================================
// IoUringExecutor Unit Tests
// ============================================================================
//
// These tests mirror executor_test.cpp for the LibuvExecutor, testing the
// io_uring-based executor implementation.
//

#ifdef HOTCOCO_HAS_IOURING

#include "hotcoco/io/iouring_executor.hpp"

#include "hotcoco/core/task.hpp"
#include "hotcoco/io/timer.hpp"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// Basic IoUringExecutor Tests
// ============================================================================

TEST(IoUringExecutorTest, CreateAndDestroy) {
    auto result = IoUringExecutor::Create();
    ASSERT_TRUE(result.IsOk());
    auto& executor = *result.Value();
    EXPECT_FALSE(executor.IsRunning());
}

TEST(IoUringExecutorTest, RunOnceEmpty) {
    auto result = IoUringExecutor::Create();
    ASSERT_TRUE(result.IsOk());
    auto& executor = *result.Value();
    executor.RunOnce();
    EXPECT_FALSE(executor.IsRunning());
}

TEST(IoUringExecutorTest, ScheduleAndRun) {
    auto result = IoUringExecutor::Create();
    ASSERT_TRUE(result.IsOk());
    auto& executor = *result.Value();
    bool executed = false;

    // Create a simple coroutine
    auto task = [&]() -> Task<void> {
        executed = true;
        executor.Stop();
        co_return;
    };

    // Schedule the coroutine
    auto t = task();
    executor.Schedule(t.GetHandle());

    // Run until stopped
    executor.Run();

    EXPECT_TRUE(executed);
}

TEST(IoUringExecutorTest, PostCallback) {
    auto result = IoUringExecutor::Create();
    ASSERT_TRUE(result.IsOk());
    auto& executor = *result.Value();
    bool called = false;

    executor.Post([&]() {
        called = true;
        executor.Stop();
    });

    executor.Run();
    EXPECT_TRUE(called);
}

// ============================================================================
// AsyncSleep Tests (using IORING_OP_TIMEOUT)
// ============================================================================

TEST(IoUringExecutorTest, AsyncSleepBasic) {
    auto result = IoUringExecutor::Create();
    ASSERT_TRUE(result.IsOk());
    auto& executor = *result.Value();
    bool slept = false;
    auto start = std::chrono::steady_clock::now();

    auto task = [&]() -> Task<void> {
        co_await AsyncSleep(50ms);
        slept = true;
        executor.Stop();
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();

    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(slept);
    EXPECT_GE(elapsed, 40ms);  // Allow some tolerance
}

TEST(IoUringExecutorTest, MultipleAsyncSleep) {
    auto result = IoUringExecutor::Create();
    ASSERT_TRUE(result.IsOk());
    auto& executor = *result.Value();
    std::vector<int> order;

    auto task1 = [&]() -> Task<void> {
        co_await AsyncSleep(50ms);
        order.push_back(1);
    };

    auto task2 = [&]() -> Task<void> {
        co_await AsyncSleep(25ms);
        order.push_back(2);
    };

    auto task3 = [&]() -> Task<void> {
        co_await AsyncSleep(75ms);
        order.push_back(3);
        executor.Stop();
    };

    auto t1 = task1();
    auto t2 = task2();
    auto t3 = task3();

    executor.Schedule(t1.GetHandle());
    executor.Schedule(t2.GetHandle());
    executor.Schedule(t3.GetHandle());

    executor.Run();

    // Should fire in order: 2, 1, 3
    ASSERT_EQ(order.size(), 3);
    EXPECT_EQ(order[0], 2);
    EXPECT_EQ(order[1], 1);
    EXPECT_EQ(order[2], 3);
}

TEST(IoUringExecutorTest, SequentialAsyncSleep) {
    auto result = IoUringExecutor::Create();
    ASSERT_TRUE(result.IsOk());
    auto& executor = *result.Value();
    int count = 0;
    auto start = std::chrono::steady_clock::now();

    auto task = [&]() -> Task<void> {
        co_await AsyncSleep(20ms);
        count++;
        co_await AsyncSleep(20ms);
        count++;
        co_await AsyncSleep(20ms);
        count++;
        executor.Stop();
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();

    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(count, 3);
    EXPECT_GE(elapsed, 50ms);  // At least 60ms total, with tolerance
}

// ============================================================================
// Thread-Local Executor Tests
// ============================================================================

TEST(IoUringExecutorTest, CurrentExecutor) {
    EXPECT_EQ(GetCurrentExecutor(), nullptr);

    auto result = IoUringExecutor::Create();
    ASSERT_TRUE(result.IsOk());
    auto& executor = *result.Value();
    bool checked = false;

    executor.Post([&]() {
        EXPECT_EQ(GetCurrentExecutor(), &executor);
        checked = true;
        executor.Stop();
    });

    executor.Run();
    EXPECT_TRUE(checked);
    EXPECT_EQ(GetCurrentExecutor(), nullptr);
}

// ============================================================================
// Cross-Thread Scheduling Tests
// ============================================================================

TEST(IoUringExecutorTest, CrossThreadSchedule) {
    auto result = IoUringExecutor::Create();
    ASSERT_TRUE(result.IsOk());
    auto& executor = *result.Value();
    std::atomic<bool> scheduled{false};
    std::atomic<bool> executed{false};

    // Start a coroutine that stops the executor
    auto task = [&]() -> Task<void> {
        executed = true;
        executor.Stop();
        co_return;
    };

    auto t = task();

    // Schedule from another thread
    std::thread other([&]() {
        std::this_thread::sleep_for(10ms);
        executor.Schedule(t.GetHandle());
        scheduled = true;
    });

    executor.Run();
    other.join();

    EXPECT_TRUE(scheduled);
    EXPECT_TRUE(executed);
}

TEST(IoUringExecutorTest, CrossThreadPost) {
    auto result = IoUringExecutor::Create();
    ASSERT_TRUE(result.IsOk());
    auto& executor = *result.Value();
    std::atomic<bool> posted{false};
    std::atomic<bool> called{false};

    // Post from another thread
    std::thread other([&]() {
        std::this_thread::sleep_for(10ms);
        executor.Post([&]() {
            called = true;
            executor.Stop();
        });
        posted = true;
    });

    executor.Run();
    other.join();

    EXPECT_TRUE(posted);
    EXPECT_TRUE(called);
}

#endif  // HOTCOCO_HAS_IOURING
