// ============================================================================
// Executor Unit Tests
// ============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "hotcoco/core/task.hpp"
#include "hotcoco/core/schedule_on.hpp"
#include "hotcoco/io/libuv_executor.hpp"
#include "hotcoco/io/timer.hpp"
#include "hotcoco/sync/sync_wait.hpp"

#include <vector>

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// Basic Executor Tests
// ============================================================================

TEST(ExecutorTest, CreateAndDestroy) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    EXPECT_FALSE(executor.IsRunning());
}

TEST(ExecutorTest, RunOnceEmpty) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    executor.RunOnce();
    EXPECT_FALSE(executor.IsRunning());
}

TEST(ExecutorTest, ScheduleAndRun) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
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

TEST(ExecutorTest, PostCallback) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    bool called = false;

    executor.Post([&]() {
        called = true;
        executor.Stop();
    });

    executor.Run();
    EXPECT_TRUE(called);
}

// ============================================================================
// AsyncSleep Tests
// ============================================================================

TEST(ExecutorTest, AsyncSleepBasic) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
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

TEST(ExecutorTest, MultipleAsyncSleep) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
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

TEST(ExecutorTest, SequentialAsyncSleep) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
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

TEST(ExecutorTest, CurrentExecutor) {
    EXPECT_EQ(GetCurrentExecutor(), nullptr);

    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
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
// Bug regression: ScheduleAfter must be thread-safe
// ============================================================================
// Previously, ScheduleAfter called uv_timer_init/uv_timer_start directly,
// which are NOT thread-safe. Calling from another thread caused data races.
// The fix queues timer requests and creates them on the loop thread.

#include "hotcoco/core/spawn.hpp"

TEST(ExecutorTest, ScheduleAfterFromOtherThread) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    std::atomic<int> fired{0};
    constexpr int kTimerCount = 20;

    // Start the executor on its own thread
    std::thread loop_thread([&]() {
        executor.Run();
    });

    // Give the loop time to start
    std::this_thread::sleep_for(50ms);

    // Schedule timers from THIS thread (not the loop thread)
    // This would crash or race with the old non-thread-safe implementation
    auto make_timer = [&]() -> Task<void> {
        co_await AsyncSleep(5ms);
        fired++;
    };
    for (int i = 0; i < kTimerCount; ++i) {
        Spawn(executor, make_timer());
    }

    // Wait for all timers to fire, then stop.
    // Use a generous timeout for TSan builds (10-15x slower).
    auto deadline = std::chrono::steady_clock::now() + 5000ms;
    while (fired.load() < kTimerCount &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    executor.Post([&]() { executor.Stop(); });

    loop_thread.join();

    EXPECT_EQ(fired.load(), kTimerCount)
        << "Not all timers fired; ScheduleAfter may not be thread-safe";
}

// ============================================================================
// Bug regression: running_ must be atomic for cross-thread IsRunning()
// ============================================================================
// running_ was a plain bool but IsRunning() can be called from any thread
// while Run()/Stop() modify it from other threads - a data race.

TEST(ExecutorTest, IsRunningFromOtherThread) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;

    EXPECT_FALSE(executor.IsRunning());

    // Driver task keeps the loop alive briefly
    auto driver = [&]() -> Task<void> {
        co_await AsyncSleep(100ms);
        executor.Stop();
    };
    Spawn(executor, driver());

    std::thread loop_thread([&]() {
        executor.Run();
    });

    // Give the loop time to start
    std::this_thread::sleep_for(10ms);

    // Poll IsRunning() from this thread — with a plain bool this is a data race
    bool saw_running = false;
    for (int i = 0; i < 100; ++i) {
        if (executor.IsRunning()) {
            saw_running = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }

    EXPECT_TRUE(saw_running)
        << "IsRunning() never returned true from another thread";

    loop_thread.join();

    EXPECT_FALSE(executor.IsRunning());
}

// ============================================================================
// ExecutorGuard Tests
// ============================================================================

TEST(ExecutorTest, ExecutorGuardSetsAndRestores) {
    EXPECT_EQ(GetCurrentExecutor(), nullptr);

    auto exec1_ptr = LibuvExecutor::Create().Value();
    auto& exec1 = *exec1_ptr;
    {
        ExecutorGuard guard(&exec1);
        EXPECT_EQ(GetCurrentExecutor(), &exec1);

        auto exec2_ptr = LibuvExecutor::Create().Value();
        auto& exec2 = *exec2_ptr;
        {
            ExecutorGuard guard2(&exec2);
            EXPECT_EQ(GetCurrentExecutor(), &exec2);
        }
        EXPECT_EQ(GetCurrentExecutor(), &exec1);
    }
    EXPECT_EQ(GetCurrentExecutor(), nullptr);
}

TEST(ExecutorTest, ExecutorGuardNullExecutor) {
    auto exec_ptr = LibuvExecutor::Create().Value();
    auto& exec = *exec_ptr;
    SetCurrentExecutor(&exec);

    {
        ExecutorGuard guard(nullptr);
        EXPECT_EQ(GetCurrentExecutor(), nullptr);
    }
    EXPECT_EQ(GetCurrentExecutor(), &exec);
    SetCurrentExecutor(nullptr);
}

// ============================================================================
// Additional Edge Cases
// ============================================================================

TEST(ExecutorTest, StopWithoutRun) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    executor.Stop();  // Should not crash
    EXPECT_FALSE(executor.IsRunning());
}

TEST(ExecutorTest, RunOnceMultipleCalls) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    bool called = false;

    auto task = [&]() -> Task<void> {
        called = true;
        co_return;
    };

    auto t = task();
    executor.Schedule(t.GetHandle());

    // RunOnce may need multiple iterations for libuv to process
    for (int i = 0; i < 10 && !called; ++i) {
        executor.RunOnce();
    }

    EXPECT_TRUE(called);
}

TEST(ExecutorTest, ScheduleMultiple) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    std::vector<int> results;

    auto task1 = [&]() -> Task<void> {
        results.push_back(1);
        co_return;
    };

    auto task2 = [&]() -> Task<void> {
        results.push_back(2);
        co_return;
    };

    auto task3 = [&]() -> Task<void> {
        results.push_back(3);
        executor.Stop();
        co_return;
    };

    auto t1 = task1();
    auto t2 = task2();
    auto t3 = task3();

    executor.Schedule(t1.GetHandle());
    executor.Schedule(t2.GetHandle());
    executor.Schedule(t3.GetHandle());

    executor.Run();

    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0], 1);
    EXPECT_EQ(results[1], 2);
    EXPECT_EQ(results[2], 3);
}

// ============================================================================
// Yield Tests
// ============================================================================

TEST(ExecutorTest, YieldReturnsControl) {
    auto task = []() -> Task<int> {
        co_await Yield();
        co_await Yield();
        co_await Yield();
        co_return 42;
    };

    EXPECT_EQ(SyncWait(task()), 42);
}
