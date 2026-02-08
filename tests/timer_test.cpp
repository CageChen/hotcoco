// ============================================================================
// AsyncSleep / Timer Tests
// ============================================================================

#include "hotcoco/io/timer.hpp"

#include "hotcoco/core/task.hpp"
#include "hotcoco/io/libuv_executor.hpp"
#include "hotcoco/sync/sync_wait.hpp"

#include <chrono>
#include <gtest/gtest.h>

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// Basic Timer Tests
// ============================================================================

TEST(TimerTest, AsyncSleepReturns) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    bool completed = false;

    auto task = [&]() -> Task<void> {
        ExecutorGuard guard(&executor);
        co_await AsyncSleep(20ms);
        completed = true;
        executor.Stop();
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();

    EXPECT_TRUE(completed);
}

TEST(TimerTest, AsyncSleepDuration) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;

    auto task = [&]() -> Task<void> {
        ExecutorGuard guard(&executor);
        auto start = std::chrono::steady_clock::now();
        co_await AsyncSleep(50ms);
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        EXPECT_GE(elapsed_ms, 40);  // Allow some jitter
        executor.Stop();
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();
}

TEST(TimerTest, MultipleSleeps) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    int count = 0;

    auto task = [&]() -> Task<void> {
        ExecutorGuard guard(&executor);
        co_await AsyncSleep(10ms);
        count++;
        co_await AsyncSleep(10ms);
        count++;
        co_await AsyncSleep(10ms);
        count++;
        executor.Stop();
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();

    EXPECT_EQ(count, 3);
}

TEST(TimerTest, ZeroDurationSleep) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    bool completed = false;

    auto task = [&]() -> Task<void> {
        ExecutorGuard guard(&executor);
        co_await AsyncSleep(0ms);
        completed = true;
        executor.Stop();
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();

    EXPECT_TRUE(completed);
}

TEST(TimerTest, SleepWithoutExecutorDoesNotSuspend) {
    // AsyncSleep without an executor should return immediately
    // (await_suspend returns false when no executor is set)
    bool completed = false;

    auto task = [&]() -> Task<void> {
        co_await AsyncSleep(100ms);
        completed = true;
    };

    // SyncWait resumes inline without executor context
    SyncWait(task());
    EXPECT_TRUE(completed);
}
