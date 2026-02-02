// ============================================================================
// Spawn Tests
// ============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "hotcoco/core/spawn.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/io/thread_pool_executor.hpp"

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// Basic Spawn Tests
// ============================================================================

Task<int> ComputeValue(int x) {
    co_return x * 2;
}

Task<void> SetFlag(std::atomic<bool>& flag) {
    flag = true;
    co_return;
}

TEST(SpawnTest, BasicSpawn) {
    std::atomic<bool> completed{false};
    ThreadPoolExecutor executor(2);
    
    Spawn(executor, SetFlag(completed));
    
    // Give it time to complete
    std::this_thread::sleep_for(50ms);
    executor.Stop();
    
    EXPECT_TRUE(completed.load());
}

TEST(SpawnTest, SpawnWithResult) {
    std::atomic<int> result{0};
    ThreadPoolExecutor executor(2);
    
    Spawn(executor, ComputeValue(21))
        .OnComplete([&result](int value) {
            result = value;
        });
    
    std::this_thread::sleep_for(50ms);
    executor.Stop();
    
    EXPECT_EQ(result.load(), 42);
}

Task<void> IncrementCounter(std::atomic<int>& counter) {
    counter++;
    co_return;
}

TEST(SpawnTest, MultipleSpawns) {
    std::atomic<int> counter{0};
    ThreadPoolExecutor executor(4);

    for (int i = 0; i < 10; i++) {
        Spawn(executor, IncrementCounter(counter));
    }

    std::this_thread::sleep_for(100ms);
    executor.Stop();

    EXPECT_EQ(counter.load(), 10);
}

// ============================================================================
// Error Handling Tests
// ============================================================================

// ============================================================================
// Bug regression: SpawnState race between Complete() and OnComplete()
// ============================================================================
// When a coroutine completes on a thread pool thread while OnComplete() is
// being registered from the spawning thread, the callback can be invoked
// twice (once from Complete(), once from OnComplete()) or not at all.

TEST(SpawnTest, OnCompleteRaceCondition) {
    // Run many iterations to increase the chance of hitting the race
    for (int iter = 0; iter < 100; ++iter) {
        std::atomic<int> call_count{0};
        ThreadPoolExecutor executor(4);

        // Spawn a fast task — it may complete before OnComplete is registered
        Spawn(executor, ComputeValue(21))
            .OnComplete([&call_count](int value) {
                EXPECT_EQ(value, 42);
                call_count++;
            });

        std::this_thread::sleep_for(20ms);
        executor.Stop();

        // The callback must be called exactly once, never 0 or 2
        EXPECT_EQ(call_count.load(), 1)
            << "Iteration " << iter << ": callback invoked "
            << call_count.load() << " times (expected exactly 1)";
    }
}

TEST(SpawnTest, OnCompleteVoidRaceCondition) {
    for (int iter = 0; iter < 100; ++iter) {
        std::atomic<int> call_count{0};
        ThreadPoolExecutor executor(4);

        Spawn(executor, []() -> Task<void> { co_return; }())
            .OnComplete([&call_count]() {
                call_count++;
            });

        std::this_thread::sleep_for(20ms);
        executor.Stop();

        EXPECT_EQ(call_count.load(), 1)
            << "Iteration " << iter << ": callback invoked "
            << call_count.load() << " times (expected exactly 1)";
    }
}

// ============================================================================
// Bug regression: SpawnedTask move-assignment leaks old coroutine frame
// ============================================================================
// If a SpawnedTask holding an unstarted coroutine is move-assigned over,
// the old coroutine frame is leaked because the destructor intentionally
// doesn't destroy (final_suspend handles it for started coroutines).

TEST(SpawnTest, MoveAssignmentDestroysOldFrame) {
    // Create two SpawnedTasks from lambdas that set flags
    std::atomic<bool> flag1{false};
    std::atomic<bool> flag2{false};

    auto make_task = [](std::atomic<bool>& flag) -> Task<void> {
        flag = true;
        co_return;
    };

    ThreadPoolExecutor executor(2);

    // Spawn first task but don't run it yet — just get the SpawnedTask
    auto spawned1 = WrapForSpawn(make_task(flag1));
    auto spawned2 = WrapForSpawn(make_task(flag2));

    // Move-assign spawned2 over spawned1 — old frame should be destroyed
    // If it leaks, ASan will catch it (coroutine frame is heap-allocated)
    spawned1 = std::move(spawned2);

    // Only spawned1 (originally spawned2) should be usable
    // The old handle from spawned1 should have been destroyed
    // We can't easily test for leaks without ASan, but we verify the
    // move-assignment doesn't crash and the new handle works
    auto state = std::make_shared<SpawnState<void>>();
    spawned1.SetState(state);
    executor.Schedule(spawned1.GetHandle());

    std::this_thread::sleep_for(50ms);
    executor.Stop();

    EXPECT_TRUE(flag2.load());
    // flag1 should NOT be set — that coroutine was destroyed without running
    EXPECT_FALSE(flag1.load());
}

TEST(SpawnTest, VoidTaskCompletion) {
    std::atomic<bool> completed{false};
    ThreadPoolExecutor executor(2);

    Spawn(executor, []() -> Task<void> {
        co_return;
    }()).OnComplete([&completed]() {
        completed = true;
    });

    std::this_thread::sleep_for(50ms);
    executor.Stop();

    EXPECT_TRUE(completed.load());
}

// ============================================================================
// Additional Edge Cases
// ============================================================================

TEST(SpawnTest, SpawnWithStringResult) {
    std::string result;
    ThreadPoolExecutor executor(2);

    auto task = []() -> Task<std::string> {
        co_return "spawned";
    };

    Spawn(executor, task())
        .OnComplete([&result](std::string s) {
            result = std::move(s);
        });

    std::this_thread::sleep_for(50ms);
    executor.Stop();

    EXPECT_EQ(result, "spawned");
}

TEST(SpawnTest, OnErrorAfterCompletion) {
    // Verify OnError is not called when task succeeds
    std::atomic<bool> error_called{false};
    std::atomic<bool> complete_called{false};
    ThreadPoolExecutor executor(2);

    Spawn(executor, []() -> Task<int> { co_return 42; }())
        .OnComplete([&](int) { complete_called = true; })
        .OnError([&](std::error_code) { error_called = true; });

    std::this_thread::sleep_for(50ms);
    executor.Stop();

    EXPECT_TRUE(complete_called.load());
    EXPECT_FALSE(error_called.load());
}


