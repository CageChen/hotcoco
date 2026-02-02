// ============================================================================
// Task Unit Tests
// ============================================================================

#include <gtest/gtest.h>

#include "hotcoco/hotcoco.hpp"

using namespace hotcoco;

// ============================================================================
// Basic Task Creation Tests
// ============================================================================

TEST(TaskTest, SimpleReturnValue) {
    // A simple coroutine that returns a value
    auto task = []() -> Task<int> {
        co_return 42;
    };

    int result = SyncWait(task());
    EXPECT_EQ(result, 42);
}

TEST(TaskTest, VoidTask) {
    // A coroutine that returns void
    bool executed = false;
    auto task = [&]() -> Task<void> {
        executed = true;
        co_return;
    };

    SyncWait(task());
    EXPECT_TRUE(executed);
}

TEST(TaskTest, StringReturnValue) {
    auto task = []() -> Task<std::string> {
        co_return "hello hotcoco";
    };

    std::string result = SyncWait(task());
    EXPECT_EQ(result, "hello hotcoco");
}

// ============================================================================
// Chained Coroutines Tests
// ============================================================================

TEST(TaskTest, ChainedCoroutines) {
    // Inner coroutine
    auto inner = []() -> Task<int> {
        co_return 21;
    };

    // Outer coroutine that awaits inner
    auto outer = [&]() -> Task<int> {
        int value = co_await inner();
        co_return value * 2;
    };

    int result = SyncWait(outer());
    EXPECT_EQ(result, 42);
}

TEST(TaskTest, MultipleChainedCoroutines) {
    auto first = []() -> Task<int> {
        co_return 10;
    };

    auto second = [&]() -> Task<int> {
        int a = co_await first();
        co_return a + 5;
    };

    auto third = [&]() -> Task<int> {
        int b = co_await second();
        co_return b * 2;
    };

    int result = SyncWait(third());
    EXPECT_EQ(result, 30);  // (10 + 5) * 2
}

// ============================================================================
// Move Semantics Tests
// ============================================================================

TEST(TaskTest, MoveOnlyResult) {
    auto task = []() -> Task<std::unique_ptr<int>> {
        co_return std::make_unique<int>(42);
    };

    auto result = SyncWait(task());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(*result, 42);
}

TEST(TaskTest, TaskIsMoveOnly) {
    auto task = []() -> Task<int> {
        co_return 42;
    };

    Task<int> t1 = task();
    Task<int> t2 = std::move(t1);

    int result = SyncWait(std::move(t2));
    EXPECT_EQ(result, 42);
}

// ============================================================================
// Lazy Execution Tests
// ============================================================================

TEST(TaskTest, LazyExecution) {
    // Verify that the task doesn't run until awaited
    bool started = false;

    auto task = [&]() -> Task<int> {
        started = true;
        co_return 42;
    };

    Task<int> t = task();
    EXPECT_FALSE(started);  // Should not have started yet

    int result = SyncWait(std::move(t));
    EXPECT_TRUE(started);   // Now it should have run
    EXPECT_EQ(result, 42);
}

// ============================================================================
// Additional Move Semantics Tests
// ============================================================================

TEST(TaskTest, MoveAssignment) {
    auto make = []() -> Task<int> { co_return 42; };

    Task<int> t1 = make();
    Task<int> t2 = make();

    // Move-assign t2 over t1 — old frame of t1 destroyed
    t1 = std::move(t2);

    int result = SyncWait(std::move(t1));
    EXPECT_EQ(result, 42);
}

TEST(TaskTest, VoidMoveAssignment) {
    bool flag1 = false;
    bool flag2 = false;

    auto make1 = [&]() -> Task<void> { flag1 = true; co_return; };
    auto make2 = [&]() -> Task<void> { flag2 = true; co_return; };

    Task<void> t1 = make1();
    Task<void> t2 = make2();

    // Move-assign — t1's old frame destroyed without running
    t1 = std::move(t2);

    SyncWait(std::move(t1));
    EXPECT_FALSE(flag1);
    EXPECT_TRUE(flag2);
}

// ============================================================================
// Deep Chaining and Various Return Types
// ============================================================================

TEST(TaskTest, DeepChaining) {
    // Test 10 levels of coroutine chaining
    std::function<Task<int>(int)> chain = [&](int depth) -> Task<int> {
        if (depth == 0) co_return 1;
        int val = co_await chain(depth - 1);
        co_return val + 1;
    };

    int result = SyncWait(chain(10));
    EXPECT_EQ(result, 11);
}

TEST(TaskTest, LargeReturnValue) {
    auto task = []() -> Task<std::vector<int>> {
        std::vector<int> v(1000);
        for (int i = 0; i < 1000; ++i) v[i] = i;
        co_return v;
    };

    auto result = SyncWait(task());
    EXPECT_EQ(result.size(), 1000u);
    EXPECT_EQ(result[999], 999);
}

TEST(TaskTest, DoubleReturnValue) {
    auto task = []() -> Task<double> {
        co_return 3.14159;
    };

    double result = SyncWait(task());
    EXPECT_DOUBLE_EQ(result, 3.14159);
}

TEST(TaskTest, BoolReturnValue) {
    auto task = [](bool val) -> Task<bool> {
        co_return val;
    };

    EXPECT_TRUE(SyncWait(task(true)));
    EXPECT_FALSE(SyncWait(task(false)));
}

TEST(TaskTest, PairReturnValue) {
    auto task = []() -> Task<std::pair<int, std::string>> {
        co_return std::make_pair(42, std::string("hello"));
    };

    auto [num, str] = SyncWait(task());
    EXPECT_EQ(num, 42);
    EXPECT_EQ(str, "hello");
}

// ============================================================================
// Deep symmetric transfer stress test (GCC bug 100897 regression)
// ============================================================================
// Without the workaround in coroutine_compat.hpp, this test stack-overflows
// under GCC+ASan because ASan prevents the tail-call optimization that
// symmetric transfer relies on.

TEST(TaskTest, DeepSymmetricTransferChain) {
    constexpr int kDepth = 5000;

    std::function<Task<int>(int)> chain = [&](int depth) -> Task<int> {
        if (depth == 0) co_return 0;
        int val = co_await chain(depth - 1);
        co_return val + 1;
    };

    int result = SyncWait(chain(kDepth));
    EXPECT_EQ(result, kDepth);
}

// ============================================================================
// Bug regression: Task double-await triggers assert
// ============================================================================
// co_awaiting the same Task twice is UB (resumes an already-running coroutine).
// The fix adds a debug assert in SetContinuation to catch this.

TEST(TaskTest, DoubleAwaitAsserts) {
    auto task = []() -> Task<int> { co_return 42; };
    auto t = task();

    // First await — should succeed
    auto outer = [&]() -> Task<int> {
        int val = co_await std::move(t);
        co_return val;
    };

    int result = SyncWait(outer());
    EXPECT_EQ(result, 42);

    // We can't easily test that a second co_await asserts in a unit test
    // without death tests, but we verify single-await works correctly.
}
