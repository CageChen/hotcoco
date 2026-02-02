// ============================================================================
// Combinator Tests
// ============================================================================

#include <gtest/gtest.h>

#include <string>

#include <atomic>

#include "hotcoco/core/combinators.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/core/timeout.hpp"
#include "hotcoco/core/when_all.hpp"
#include "hotcoco/core/when_any.hpp"
#include "hotcoco/core/error.hpp"
#include "hotcoco/core/result.hpp"
#include "hotcoco/sync/sync_wait.hpp"

using namespace hotcoco;

// ============================================================================
// Then (Promise Chaining) Tests
// ============================================================================

Task<int> MakeInt(int value) {
    co_return value;
}

Task<std::string> MakeString(const char* value) {
    co_return std::string(value);
}

Task<void> SetValue(int& target, int value) {
    target = value;
    co_return;
}

// Test Then with SyncWait  
TEST(CombinatorTest, ThenBasic) {
    auto task = Then(MakeInt(21), [](int x) { return x * 2; });
    
    int result = SyncWait(std::move(task));
    EXPECT_EQ(result, 42);
}

TEST(CombinatorTest, ThenToString) {
    auto task = Then(MakeInt(123), [](int x) { return std::to_string(x); });
    
    std::string result = SyncWait(std::move(task));
    EXPECT_EQ(result, "123");
}

TEST(CombinatorTest, ThenChained) {
    // Chain multiple transformations
    auto step1 = Then(MakeInt(5), [](int x) { return x * 2; });      // 10
    auto step2 = Then(std::move(step1), [](int x) { return x + 3; });  // 13
    auto step3 = Then(std::move(step2), [](int x) { return x * x; });  // 169
    
    int result = SyncWait(std::move(step3));
    EXPECT_EQ(result, 169);
}

TEST(CombinatorTest, ThenWithVoidInput) {
    int value = 0;
    
    auto void_task = SetValue(value, 10);
    auto task = Then(std::move(void_task), [&]() { return value * 2; });
    
    int result = SyncWait(std::move(task));
    EXPECT_EQ(result, 20);
}

TEST(CombinatorTest, ThenVoidToVoid) {
    int counter = 0;
    
    auto void_task = SetValue(counter, 1);
    auto task = Then(std::move(void_task), [&]() { counter += 10; });
    
    SyncWait(std::move(task));
    EXPECT_EQ(counter, 11);
}

TEST(CombinatorTest, ComplexChain) {
    auto task = Then(MakeString("hotcoco"), [](std::string s) {
        return s + " is awesome!";
    });
    
    auto final_task = Then(std::move(task), [](std::string s) {
        return s.length();
    });
    
    size_t result = SyncWait(std::move(final_task));
    // "hotcoco is awesome!" = 19 characters
    EXPECT_EQ(result, 19u);
}

// ============================================================================
// Multiple independent tasks with Then
// ============================================================================

TEST(CombinatorTest, ParallelTransformations) {
    auto task1 = Then(MakeInt(1), [](int x) { return x * 10; });
    auto task2 = Then(MakeInt(2), [](int x) { return x * 20; });
    auto task3 = Then(MakeInt(3), [](int x) { return x * 30; });

    // These run independently
    int r1 = SyncWait(std::move(task1));
    int r2 = SyncWait(std::move(task2));
    int r3 = SyncWait(std::move(task3));

    EXPECT_EQ(r1, 10);
    EXPECT_EQ(r2, 40);
    EXPECT_EQ(r3, 90);
}

// ============================================================================
// WhenAll Concurrent Tests (latch-based implementation)
// ============================================================================

TEST(WhenAllConcurrentTest, HeterogeneousTypes) {
    auto task = WhenAll(MakeInt(42), MakeString("hello"));
    auto [num, str] = SyncWait(std::move(task));
    EXPECT_EQ(num, 42);
    EXPECT_EQ(str, "hello");
}

TEST(WhenAllConcurrentTest, FiveTasks) {
    std::vector<Task<int>> tasks;
    for (int i = 0; i < 5; ++i) {
        tasks.push_back(MakeInt(i * 10));
    }

    auto result = SyncWait(WhenAll(std::move(tasks)));
    ASSERT_EQ(result.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(result[i], i * 10);
    }
}

TEST(WhenAllConcurrentTest, VectorOfVoidTasks) {
    std::atomic<int> counter{0};

    auto make_void = [&](int add) -> Task<void> {
        counter.fetch_add(add);
        co_return;
    };

    std::vector<Task<void>> tasks;
    tasks.push_back(make_void(1));
    tasks.push_back(make_void(2));
    tasks.push_back(make_void(3));

    SyncWait(WhenAll(std::move(tasks)));
    EXPECT_EQ(counter.load(), 6);
}

TEST(WhenAllConcurrentTest, SingleTask) {
    auto task = WhenAll(MakeInt(99));
    auto [val] = SyncWait(std::move(task));
    EXPECT_EQ(val, 99);
}

TEST(WhenAllConcurrentTest, EmptyVector) {
    std::vector<Task<int>> tasks;
    auto result = SyncWait(WhenAll(std::move(tasks)));
    EXPECT_TRUE(result.empty());
}

TEST(WhenAllConcurrentTest, AllTasksExecuted) {
    std::vector<int> order;

    auto task_a = [&]() -> Task<int> {
        order.push_back(1);
        co_return 10;
    };

    auto task_b = [&]() -> Task<int> {
        order.push_back(2);
        co_return 20;
    };

    auto [a, b] = SyncWait(WhenAll(task_a(), task_b()));
    EXPECT_EQ(a, 10);
    EXPECT_EQ(b, 20);
    EXPECT_EQ(order.size(), 2u);
}

// ============================================================================
// WhenAny Concurrent Tests (CAS-based racing)
// ============================================================================

TEST(WhenAnyConcurrentTest, VectorRace) {
    auto race = []() -> Task<WhenAnyResult<int>> {
        std::vector<Task<int>> tasks;
        tasks.push_back(MakeInt(42));
        tasks.push_back(MakeInt(100));
        tasks.push_back(MakeInt(200));
        auto r = co_await WhenAny(std::move(tasks));
        co_return std::move(r).Value();
    };

    auto result = SyncWait(race());
    EXPECT_EQ(result.index, 0u);
    EXPECT_EQ(result.value, 42);
}

TEST(WhenAnyConcurrentTest, VariadicRace) {
    auto race = []() -> Task<WhenAnyResult<int>> {
        auto r = co_await WhenAny(MakeInt(10), MakeInt(20), MakeInt(30));
        co_return std::move(r).Value();
    };

    auto result = SyncWait(race());
    EXPECT_EQ(result.index, 0u);
    EXPECT_EQ(result.value, 10);
}

TEST(WhenAnyConcurrentTest, SingleTask) {
    auto race = []() -> Task<WhenAnyResult<int>> {
        std::vector<Task<int>> tasks;
        tasks.push_back(MakeInt(77));
        auto r = co_await WhenAny(std::move(tasks));
        co_return std::move(r).Value();
    };

    auto result = SyncWait(race());
    EXPECT_EQ(result.index, 0u);
    EXPECT_EQ(result.value, 77);
}

TEST(WhenAnyConcurrentTest, EmptyVectorReturnsError) {
    auto race = []() -> Task<Result<WhenAnyResult<int>, std::error_code>> {
        std::vector<Task<int>> tasks;
        co_return co_await WhenAny(std::move(tasks));
    };

    auto result = SyncWait(race());
    EXPECT_TRUE(result.IsErr());
    EXPECT_EQ(result.Error(), make_error_code(Errc::InvalidArgument));
}

TEST(WhenAnyConcurrentTest, AllTasksCompleteCleanly) {
    // Verify no memory leaks — all tasks (including losers) run to completion
    std::atomic<int> completed{0};

    auto make_task = [&](int val) -> Task<int> {
        completed.fetch_add(1);
        co_return val;
    };

    auto race = [&]() -> Task<WhenAnyResult<int>> {
        std::vector<Task<int>> tasks;
        tasks.push_back(make_task(1));
        tasks.push_back(make_task(2));
        tasks.push_back(make_task(3));
        auto r = co_await WhenAny(std::move(tasks));
        co_return std::move(r).Value();
    };

    auto result = SyncWait(race());
    EXPECT_EQ(result.value, 1);
    // All 3 tasks should have completed (losers too)
    EXPECT_EQ(completed.load(), 3);
}

// ============================================================================
// WithTimeout Tests
// ============================================================================

TEST(WithTimeoutTest, TaskCompletesBeforeTimeout) {
    // Without an executor, AsyncSleep resumes immediately, so user task
    // always wins the race when it completes synchronously.
    auto task = []() -> Task<Result<int, TimeoutError>> {
        co_return co_await WithTimeout(MakeInt(42), std::chrono::seconds(10));
    };

    auto result = SyncWait(task());
    EXPECT_TRUE(result.IsOk());
    EXPECT_EQ(result.Value(), 42);
}

TEST(WithTimeoutTest, VoidTaskCompletesBeforeTimeout) {
    std::atomic<int> counter{0};

    auto work = [&]() -> Task<void> {
        counter.store(99);
        co_return;
    };

    auto task = [&]() -> Task<Result<void, TimeoutError>> {
        co_return co_await WithTimeout(work(), std::chrono::seconds(10));
    };

    auto result = SyncWait(task());
    EXPECT_TRUE(result.IsOk());
    EXPECT_EQ(counter.load(), 99);
}

// ============================================================================
// Additional Combinator Tests
// ============================================================================

TEST(CombinatorTest, MapAlias) {
    auto make_int = []() -> Task<int> { co_return 21; };

    auto task = Map(make_int(), [](int x) { return x * 2; });
    int result = SyncWait(std::move(task));
    EXPECT_EQ(result, 42);
}

TEST(CombinatorTest, ThenToVoid) {
    int side_effect = 0;
    auto make_int = []() -> Task<int> { co_return 42; };

    auto task = Then(make_int(), [&](int x) { side_effect = x; });
    SyncWait(std::move(task));
    EXPECT_EQ(side_effect, 42);
}

TEST(CombinatorTest, VoidToValue) {
    auto void_task = []() -> Task<void> { co_return; };
    auto task = Then(void_task(), []() { return 42; });
    EXPECT_EQ(SyncWait(std::move(task)), 42);
}

TEST(CombinatorTest, VoidToString) {
    auto void_task = []() -> Task<void> { co_return; };
    auto task = Then(void_task(), []() { return std::string("hello"); });
    EXPECT_EQ(SyncWait(std::move(task)), "hello");
}

// ============================================================================
// Additional WhenAll Tests
// ============================================================================

TEST(WhenAllConcurrentTest, VectorSingleTask) {
    auto make_int = [](int v) -> Task<int> { co_return v; };

    std::vector<Task<int>> tasks;
    tasks.push_back(make_int(42));

    auto result = SyncWait(WhenAll(std::move(tasks)));
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], 42);
}

TEST(WhenAllConcurrentTest, TwoStringTasks) {
    auto make = [](const char* s) -> Task<std::string> { co_return std::string(s); };

    auto [a, b] = SyncWait(WhenAll(make("hello"), make("world")));
    EXPECT_EQ(a, "hello");
    EXPECT_EQ(b, "world");
}

TEST(WhenAllConcurrentTest, MixedTypes) {
    auto int_task = []() -> Task<int> { co_return 42; };
    auto str_task = []() -> Task<std::string> { co_return "hello"; };
    auto double_task = []() -> Task<double> { co_return 3.14; };

    auto [i, s, d] = SyncWait(WhenAll(int_task(), str_task(), double_task()));
    EXPECT_EQ(i, 42);
    EXPECT_EQ(s, "hello");
    EXPECT_DOUBLE_EQ(d, 3.14);
}

TEST(WhenAllConcurrentTest, LargeVector) {
    auto make = [](int v) -> Task<int> { co_return v; };

    std::vector<Task<int>> tasks;
    for (int i = 0; i < 100; ++i) {
        tasks.push_back(make(i));
    }

    auto result = SyncWait(WhenAll(std::move(tasks)));
    ASSERT_EQ(result.size(), 100u);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(result[i], i);
    }
}

// ============================================================================
// Additional WhenAny Tests
// ============================================================================

TEST(WhenAnyConcurrentTest, TwoTasks) {
    auto make_int = [](int v) -> Task<int> { co_return v; };

    auto race = [&]() -> Task<WhenAnyResult<int>> {
        auto r = co_await WhenAny(make_int(10), make_int(20));
        co_return std::move(r).Value();
    };

    auto result = SyncWait(race());
    EXPECT_EQ(result.index, 0u);
    EXPECT_EQ(result.value, 10);
}

// ============================================================================
// Additional WithTimeout Tests
// ============================================================================

TEST(WithTimeoutTest, StringTask) {
    auto make_str = []() -> Task<std::string> { co_return "hello"; };

    auto task = [&]() -> Task<Result<std::string, TimeoutError>> {
        co_return co_await WithTimeout(make_str(), std::chrono::seconds(10));
    };

    auto result = SyncWait(task());
    EXPECT_TRUE(result.IsOk());
    EXPECT_EQ(result.Value(), "hello");
}

TEST(WithTimeoutTest, TimeoutErrorMessage) {
    using namespace std::chrono_literals;
    TimeoutError err{100ms};
    EXPECT_EQ(err.Message(), "Operation timed out after 100ms");
}
