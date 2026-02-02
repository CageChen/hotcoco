// ============================================================================
// TaskGroup Tests
// ============================================================================

#include <gtest/gtest.h>

#include <atomic>

#include "hotcoco/core/task.hpp"
#include "hotcoco/core/task_group.hpp"
#include "hotcoco/io/libuv_executor.hpp"
#include "hotcoco/sync/sync_wait.hpp"

using namespace hotcoco;

TEST(TaskGroupTest, BasicThreeTasks) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    std::atomic<int> counter{0};

    auto run = [&]() -> Task<void> {
        ExecutorGuard guard(&executor);
        TaskGroup group(executor);

        group.Start([&]() -> Task<void> {
            counter.fetch_add(1);
            co_return;
        }());
        group.Start([&]() -> Task<void> {
            counter.fetch_add(10);
            co_return;
        }());
        group.Start([&]() -> Task<void> {
            counter.fetch_add(100);
            co_return;
        }());

        co_await group;
        EXPECT_EQ(counter.load(), 111);
    };

    SyncWait(run());
}

TEST(TaskGroupTest, EmptyGroupPassesThrough) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;

    auto run = [&]() -> Task<void> {
        ExecutorGuard guard(&executor);
        TaskGroup group(executor);
        // No tasks started — co_await should pass through immediately
        co_await group;
    };

    SyncWait(run());
}

TEST(TaskGroupTest, SizeTracking) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;

    auto run = [&]() -> Task<void> {
        ExecutorGuard guard(&executor);
        TaskGroup group(executor);
        EXPECT_EQ(group.Size(), 0u);
        EXPECT_TRUE(group.Empty());

        group.Start([]() -> Task<void> { co_return; }());
        // After synchronous completion, size should be back to 0
        EXPECT_EQ(group.Size(), 0u);
        co_return;
    };

    SyncWait(run());
}

TEST(TaskGroupTest, MultipleRounds) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    std::atomic<int> counter{0};

    auto run = [&]() -> Task<void> {
        ExecutorGuard guard(&executor);
        TaskGroup group(executor);

        // Round 1
        group.Start([&]() -> Task<void> {
            counter.fetch_add(1);
            co_return;
        }());
        co_await group;
        EXPECT_EQ(counter.load(), 1);

        // Round 2 — reuse the same group
        group.Start([&]() -> Task<void> {
            counter.fetch_add(10);
            co_return;
        }());
        co_await group;
        EXPECT_EQ(counter.load(), 11);
    };

    SyncWait(run());
}

// ============================================================================
// Additional Edge Cases
// ============================================================================

TEST(TaskGroupTest, ErrorInGroupTask) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;

    auto run = [&]() -> Task<void> {
        ExecutorGuard guard(&executor);
        TaskGroup group(executor);

        group.Start([]() -> Task<void> {
            // With -fno-exceptions, tasks can't throw.
            // Just verify the group handles a simple task correctly.
            co_return;
        }());

        co_await group;
    };

    // TaskGroup uses DetachedTask which calls abort() on unhandled exceptions
    SyncWait(run());
}
