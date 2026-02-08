// ============================================================================
// PollingExecutor Unit Tests
// ============================================================================

#include "hotcoco/io/polling_executor.hpp"

#include "hotcoco/core/result.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/io/timer.hpp"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// Mock CompletionSource for Testing
// ============================================================================

class MockCompletionSource : public CompletionSource {
   public:
    // Queue handles to be returned by Poll()
    void QueueHandle(std::coroutine_handle<> h) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push(h);
    }

    size_t Poll(std::span<std::coroutine_handle<>> out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        while (!pending_.empty() && count < out.size()) {
            out[count++] = pending_.front();
            pending_.pop();
        }
        return count;
    }

   private:
    std::mutex mutex_;
    std::queue<std::coroutine_handle<>> pending_;
};

// ============================================================================
// Basic PollingExecutor Tests
// ============================================================================

TEST(PollingExecutorTest, CreateAndDestroy) {
    auto source = std::make_unique<MockCompletionSource>();
    auto executor_result = PollingExecutor::Create(std::move(source));
    ASSERT_TRUE(executor_result.IsOk());
    auto& executor = *executor_result.Value();
    EXPECT_FALSE(executor.IsRunning());
}

TEST(PollingExecutorTest, RunOnceEmpty) {
    auto source = std::make_unique<MockCompletionSource>();
    auto executor_result = PollingExecutor::Create(std::move(source));
    ASSERT_TRUE(executor_result.IsOk());
    auto& executor = *executor_result.Value();
    executor.RunOnce();
    EXPECT_FALSE(executor.IsRunning());
}

TEST(PollingExecutorTest, ScheduleAndRun) {
    auto source = std::make_unique<MockCompletionSource>();
    auto executor_result = PollingExecutor::Create(std::move(source));
    ASSERT_TRUE(executor_result.IsOk());
    auto& executor = *executor_result.Value();
    bool executed = false;

    auto task = [&]() -> Task<void> {
        executed = true;
        executor.Stop();
        co_return;
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();

    EXPECT_TRUE(executed);
}

TEST(PollingExecutorTest, PostCallback) {
    auto source = std::make_unique<MockCompletionSource>();
    auto executor_result = PollingExecutor::Create(std::move(source));
    ASSERT_TRUE(executor_result.IsOk());
    auto& executor = *executor_result.Value();
    bool called = false;

    executor.Post([&]() {
        called = true;
        executor.Stop();
    });

    executor.Run();
    EXPECT_TRUE(called);
}

// ============================================================================
// CompletionSource Integration Tests
// ============================================================================

TEST(PollingExecutorTest, CompletionSourceReturns) {
    auto source = std::make_unique<MockCompletionSource>();
    auto* source_ptr = source.get();
    auto executor_result = PollingExecutor::Create(std::move(source));
    ASSERT_TRUE(executor_result.IsOk());
    auto& executor = *executor_result.Value();

    bool completed = false;

    auto task = [&]() -> Task<void> {
        completed = true;
        executor.Stop();
        co_return;
    };

    auto t = task();

    // Instead of using Schedule, queue directly to completion source
    source_ptr->QueueHandle(t.GetHandle());

    executor.Run();
    EXPECT_TRUE(completed);
}

// ============================================================================
// Timer Tests (ScheduleAfter)
// ============================================================================

TEST(PollingExecutorTest, ScheduleAfterBasic) {
    auto source = std::make_unique<MockCompletionSource>();
    auto executor_result = PollingExecutor::Create(std::move(source));
    ASSERT_TRUE(executor_result.IsOk());
    auto& executor = *executor_result.Value();

    bool scheduled = false;
    auto start = std::chrono::steady_clock::now();

    auto task = [&]() -> Task<void> {
        co_await AsyncSleep(50ms);
        scheduled = true;
        executor.Stop();
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();

    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(scheduled);
    EXPECT_GE(elapsed, 40ms);  // Allow tolerance
}

TEST(PollingExecutorTest, MultipleScheduleAfter) {
    auto source = std::make_unique<MockCompletionSource>();
    auto executor_result = PollingExecutor::Create(std::move(source));
    ASSERT_TRUE(executor_result.IsOk());
    auto& executor = *executor_result.Value();

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

    ASSERT_EQ(order.size(), 3);
    EXPECT_EQ(order[0], 2);  // 25ms first
    EXPECT_EQ(order[1], 1);  // 50ms second
    EXPECT_EQ(order[2], 3);  // 75ms third
}

// ============================================================================
// Thread-Local Executor Tests
// ============================================================================

TEST(PollingExecutorTest, CurrentExecutor) {
    EXPECT_EQ(GetCurrentExecutor(), nullptr);

    auto source = std::make_unique<MockCompletionSource>();
    auto executor_result = PollingExecutor::Create(std::move(source));
    ASSERT_TRUE(executor_result.IsOk());
    auto& executor = *executor_result.Value();
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

TEST(PollingExecutorTest, CrossThreadSchedule) {
    auto source = std::make_unique<MockCompletionSource>();
    auto executor_result = PollingExecutor::Create(std::move(source));
    ASSERT_TRUE(executor_result.IsOk());
    auto& executor = *executor_result.Value();

    std::atomic<bool> scheduled{false};
    std::atomic<bool> executed{false};

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

TEST(PollingExecutorTest, CrossThreadPost) {
    auto source = std::make_unique<MockCompletionSource>();
    auto executor_result = PollingExecutor::Create(std::move(source));
    ASSERT_TRUE(executor_result.IsOk());
    auto& executor = *executor_result.Value();

    std::atomic<bool> posted{false};
    std::atomic<bool> called{false};

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

// ============================================================================
// Idle Strategy Tests
// ============================================================================

// ============================================================================
// Bug regression: ProcessTimers must not hold mutex while resuming coroutines
// ============================================================================
// A timer-expired coroutine that calls Schedule() would deadlock because
// ProcessTimers held queue_mutex_ while resuming, and Schedule also acquires it.

TEST(PollingExecutorTest, TimerCallbackSchedules) {
    auto source = std::make_unique<MockCompletionSource>();
    auto executor_result = PollingExecutor::Create(std::move(source));
    ASSERT_TRUE(executor_result.IsOk());
    auto& executor = *executor_result.Value();

    bool first_done = false;
    bool second_done = false;

    // A coroutine that, when resumed from a timer, schedules another coroutine
    auto inner_task = [&]() -> Task<void> {
        second_done = true;
        executor.Stop();
        co_return;
    };

    auto outer_task = [&]() -> Task<void> {
        co_await AsyncSleep(10ms);
        first_done = true;
        // This calls Schedule() -- would deadlock if ProcessTimers holds the mutex
        auto t = inner_task();
        executor.Schedule(t.GetHandle());
        // Keep t alive until inner completes
        co_await AsyncSleep(1000ms);  // Will be stopped before this fires
    };

    auto t = outer_task();
    executor.Schedule(t.GetHandle());

    // Run with a timeout to detect deadlock
    std::atomic<bool> finished{false};
    std::thread runner([&]() {
        executor.Run();
        finished = true;
    });

    auto start = std::chrono::steady_clock::now();
    while (!finished.load()) {
        std::this_thread::sleep_for(10ms);
        if (std::chrono::steady_clock::now() - start > 2s) {
            executor.Stop();
            runner.join();
            FAIL() << "Timeout: ProcessTimers likely deadlocked";
        }
    }
    runner.join();

    EXPECT_TRUE(first_done);
    EXPECT_TRUE(second_done);
}

TEST(PollingExecutorTest, SleepIdleStrategy) {
    auto source = std::make_unique<MockCompletionSource>();
    PollingExecutor::Options opts;
    opts.idle_strategy = PollingExecutor::Options::IdleStrategy::Sleep;
    opts.sleep_duration = std::chrono::microseconds(100);

    auto executor_result = PollingExecutor::Create(std::move(source), opts);
    ASSERT_TRUE(executor_result.IsOk());
    auto& executor = *executor_result.Value();

    executor.Post([&]() { executor.Stop(); });

    executor.Run();
    // Just verify it doesn't hang
}
