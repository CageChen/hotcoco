// ============================================================================
// Invoke Tests
// ============================================================================

#include "hotcoco/core/invoke.hpp"

#include "hotcoco/core/task.hpp"
#include "hotcoco/sync/sync_wait.hpp"

#include <gtest/gtest.h>
#include <string>

using namespace hotcoco;

// ============================================================================
// Basic Invoke Tests
// ============================================================================

TEST(InvokeTest, BasicLambdaReturnsInt) {
    auto make_task = []() -> Task<int> { co_return 42; };

    int result = SyncWait(Invoke(make_task));
    EXPECT_EQ(result, 42);
}

TEST(InvokeTest, LambdaWithArgs) {
    auto make_task = [](int a, int b) -> Task<int> { co_return a + b; };

    int result = SyncWait(Invoke(make_task, 10, 32));
    EXPECT_EQ(result, 42);
}

TEST(InvokeTest, LambdaReturnsString) {
    auto make_task = [](const char* prefix) -> Task<std::string> { co_return std::string(prefix) + " world"; };

    std::string result = SyncWait(Invoke(make_task, "hello"));
    EXPECT_EQ(result, "hello world");
}

TEST(InvokeTest, VoidLambda) {
    int counter = 0;

    auto make_task = [&counter]() -> Task<void> {
        counter = 99;
        co_return;
    };

    SyncWait(Invoke(make_task));
    EXPECT_EQ(counter, 99);
}

// ============================================================================
// Safe Capture Tests — the core purpose of Invoke
// ============================================================================

TEST(InvokeTest, CaptureByValueIsSafe) {
    // The lambda captures 'value' by value. Even if the original variable
    // is destroyed, the copy on the coroutine frame remains valid.
    auto make_task = [](int value) -> Task<int> { co_return value * 2; };

    int result = SyncWait(Invoke(make_task, 21));
    EXPECT_EQ(result, 42);
}

TEST(InvokeTest, FunctorCopiedOntoFrame) {
    // Verify the functor itself is copied onto the coroutine frame.
    // We use a stateful functor to test this.
    struct Adder {
        int base;
        Task<int> operator()(int x) const { co_return base + x; }
    };

    Adder adder{100};
    int result = SyncWait(Invoke(adder, 23));
    EXPECT_EQ(result, 123);
}

TEST(InvokeTest, MultipleInvocations) {
    auto make_task = [](int x) -> Task<int> { co_return x* x; };

    int r1 = SyncWait(Invoke(make_task, 3));
    int r2 = SyncWait(Invoke(make_task, 7));
    int r3 = SyncWait(Invoke(make_task, 10));

    EXPECT_EQ(r1, 9);
    EXPECT_EQ(r2, 49);
    EXPECT_EQ(r3, 100);
}

TEST(InvokeTest, LambdaWithMoveOnlyArg) {
    auto make_task = [](std::unique_ptr<int> p) -> Task<int> { co_return *p; };

    auto ptr = std::make_unique<int>(42);
    int result = SyncWait(Invoke(make_task, std::move(ptr)));
    EXPECT_EQ(result, 42);
}

// ============================================================================
// Additional Edge Cases
// ============================================================================

TEST(InvokeTest, VoidWithArgs) {
    int result = 0;
    auto make_task = [&](int a, int b) -> Task<void> {
        result = a + b;
        co_return;
    };

    SyncWait(Invoke(make_task, 10, 32));
    EXPECT_EQ(result, 42);
}
