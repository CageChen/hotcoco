// ============================================================================
// DetachedTask Tests
// ============================================================================

#include "hotcoco/core/detached_task.hpp"

#include "hotcoco/core/task.hpp"

#include <atomic>
#include <gtest/gtest.h>

using namespace hotcoco;

// ============================================================================
// Basic functionality
// ============================================================================

TEST(DetachedTaskTest, BasicCompletion) {
    std::atomic<bool> completed{false};

    auto task = []() -> Task<void> { co_return; };

    auto detached = MakeDetached(task());
    detached.SetCallback([&]() { completed.store(true); });
    detached.Start();

    EXPECT_TRUE(completed.load());
}

TEST(DetachedTaskTest, CallbackInvokedOnCompletion) {
    int callback_value = 0;

    auto task = []() -> Task<void> { co_return; };

    auto detached = MakeDetached(task());
    detached.SetCallback([&]() { callback_value = 42; });
    detached.Start();

    EXPECT_EQ(callback_value, 42);
}

TEST(DetachedTaskTest, NoCallbackIsOk) {
    auto task = []() -> Task<void> { co_return; };

    auto detached = MakeDetached(task());
    // No SetCallback - should not crash
    detached.Start();
}

TEST(DetachedTaskTest, UnstartedTaskDestroyedSafely) {
    auto task = []() -> Task<void> { co_return; };

    {
        auto detached = MakeDetached(task());
        // Goes out of scope without Start() - should not leak
    }
}

TEST(DetachedTaskTest, MoveSemantics) {
    std::atomic<bool> completed{false};

    auto task = []() -> Task<void> { co_return; };

    auto detached1 = MakeDetached(task());
    detached1.SetCallback([&]() { completed.store(true); });

    auto detached2 = std::move(detached1);
    detached2.Start();

    EXPECT_TRUE(completed.load());
}

TEST(DetachedTaskTest, MultipleDetachedTasks) {
    std::atomic<int> count{0};

    auto make_task = []() -> Task<void> { co_return; };

    for (int i = 0; i < 10; ++i) {
        auto detached = MakeDetached(make_task());
        detached.SetCallback([&]() { count.fetch_add(1); });
        detached.Start();
    }

    EXPECT_EQ(count.load(), 10);
}

TEST(DetachedTaskTest, WithInnerWork) {
    std::atomic<int> value{0};

    auto inner = [&]() -> Task<void> {
        value.store(10);
        co_return;
    };

    auto detached = MakeDetached(inner());
    detached.SetCallback([&]() { value.fetch_add(5); });
    detached.Start();

    EXPECT_EQ(value.load(), 15);
}

// ============================================================================
// Additional Edge Cases
// ============================================================================

TEST(DetachedTaskTest, ChainedTask) {
    std::atomic<int> value{0};

    auto inner = [&]() -> Task<void> {
        value.store(10);
        co_return;
    };

    auto outer = [&]() -> Task<void> {
        co_await inner();
        value.fetch_add(5);
    };

    auto detached = MakeDetached(outer());
    detached.Start();
    EXPECT_EQ(value.load(), 15);
}

TEST(DetachedTaskTest, MoveAssignment) {
    std::atomic<int> value1{0};
    std::atomic<int> value2{0};

    auto make1 = [&]() -> Task<void> {
        value1 = 1;
        co_return;
    };
    auto make2 = [&]() -> Task<void> {
        value2 = 2;
        co_return;
    };

    auto t1 = MakeDetached(make1());
    auto t2 = MakeDetached(make2());

    t1 = std::move(t2);
    t1.Start();

    // Only t2's work should have run
    EXPECT_EQ(value1.load(), 0);
    EXPECT_EQ(value2.load(), 2);
}
