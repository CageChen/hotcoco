// ============================================================================
// Timeout Tests
// ============================================================================

#include <gtest/gtest.h>

#include "hotcoco/core/task.hpp"
#include "hotcoco/core/timeout.hpp"
#include "hotcoco/io/libuv_executor.hpp"
#include "hotcoco/sync/sync_wait.hpp"

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// Basic Timeout Tests
// ============================================================================

TEST(TimeoutTest, TaskCompletesBeforeTimeout) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;

    auto task = [&]() -> Task<void> {
        ExecutorGuard guard(&executor);

        // Use a short timeout so the timer child inside WithTimeout's
        // DetachedTask controller finishes quickly after the user task wins.
        // This avoids leaking coroutine frames when the executor stops.
        auto result = co_await WithTimeout(
            []() -> Task<int> { co_return 42; }(),
            10ms);

        EXPECT_TRUE(result.IsOk());
        EXPECT_EQ(result.Value(), 42);

        // Give the timer child time to fire and WhenAll to complete,
        // so the DetachedTask controller self-destructs cleanly.
        co_await AsyncSleep(30ms);
        executor.Stop();
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();
}

TEST(TimeoutTest, VoidTaskCompletesBeforeTimeout) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;

    auto task = [&]() -> Task<void> {
        ExecutorGuard guard(&executor);

        // Use a short timeout so the timer child inside WithTimeout's
        // DetachedTask controller finishes quickly after the user task wins.
        auto result = co_await WithTimeout(
            []() -> Task<void> { co_return; }(),
            10ms);

        EXPECT_TRUE(result.IsOk());

        // Give the timer child time to fire and WhenAll to complete,
        // so the DetachedTask controller self-destructs cleanly.
        co_await AsyncSleep(30ms);
        executor.Stop();
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();
}

TEST(TimeoutTest, TaskTimesOut) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;

    auto task = [&]() -> Task<void> {
        ExecutorGuard guard(&executor);

        // Use a short user-task sleep so the coroutine frame is cleaned up
        // promptly after the timeout fires, avoiding ASan leak reports.
        auto result = co_await WithTimeout(
            [&]() -> Task<int> {
                co_await AsyncSleep(200ms);
                co_return 42;
            }(),
            50ms);

        EXPECT_TRUE(result.IsErr());
        auto& err = result.Error();
        EXPECT_GE(err.duration.count(), 50);
        EXPECT_FALSE(err.Message().empty());

        // Give the losing user-task child time to complete so
        // WhenAll finishes and the DetachedTask self-destructs.
        co_await AsyncSleep(300ms);
        executor.Stop();
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();
}

TEST(TimeoutTest, TimeoutErrorMessage) {
    TimeoutError err{500ms};
    EXPECT_EQ(err.Message(), "Operation timed out after 500ms");
}
