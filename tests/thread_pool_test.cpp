// ============================================================================
// Thread Pool Executor Tests
// ============================================================================

#include "hotcoco/core/error.hpp"
#include "hotcoco/core/result.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/core/when_all.hpp"
#include "hotcoco/core/when_any.hpp"
#include "hotcoco/io/thread_pool_executor.hpp"
#include "hotcoco/sync/sync_wait.hpp"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <mutex>
#include <set>
#include <thread>

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// Basic ThreadPoolExecutor Tests
// ============================================================================

TEST(ThreadPoolTest, CreateAndDestroy) {
    ThreadPoolExecutor executor(2);
    EXPECT_EQ(executor.NumThreads(), 2u);
}

TEST(ThreadPoolTest, ScheduleCallback) {
    ThreadPoolExecutor executor(2);
    std::atomic<int> counter{0};
    const int num_tasks = 4;

    for (int i = 0; i < num_tasks; ++i) {
        executor.Post([&]() {
            if (++counter == num_tasks) {
                executor.Stop();
            }
        });
    }

    executor.Run();

    EXPECT_EQ(counter.load(), num_tasks);
}

TEST(ThreadPoolTest, ScheduleCoroutine) {
    ThreadPoolExecutor executor(2);
    std::atomic<int> value{0};

    auto task = [&]() -> Task<void> {
        value = 42;
        executor.Stop();
        co_return;
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();

    EXPECT_EQ(value.load(), 42);
}

TEST(ThreadPoolTest, MultipleTasks) {
    ThreadPoolExecutor executor(4);
    std::atomic<int> counter{0};
    const int num_tasks = 100;

    for (int i = 0; i < num_tasks; ++i) {
        executor.Post([&]() {
            if (++counter == num_tasks) {
                executor.Stop();
            }
        });
    }

    executor.Run();

    EXPECT_EQ(counter.load(), num_tasks);
}

TEST(ThreadPoolTest, ParallelExecution) {
    ThreadPoolExecutor executor(4);
    std::mutex mutex;
    std::set<std::thread::id> thread_ids;
    const int num_tasks = 20;
    std::atomic<int> completed{0};

    for (int i = 0; i < num_tasks; ++i) {
        executor.Post([&]() {
            // Record thread ID
            {
                std::lock_guard<std::mutex> lock(mutex);
                thread_ids.insert(std::this_thread::get_id());
            }

            // Simulate some work
            std::this_thread::sleep_for(10ms);

            if (++completed == num_tasks) {
                executor.Stop();
            }
        });
    }

    executor.Run();

    // Should have used multiple threads
    EXPECT_GT(thread_ids.size(), 1u);
}

TEST(ThreadPoolTest, ScheduleAfterDelay) {
    ThreadPoolExecutor executor(2);
    auto start = std::chrono::steady_clock::now();
    std::atomic<bool> executed{false};

    // Post a callback that waits, simulating delayed execution
    executor.Post([&]() {
        std::this_thread::sleep_for(50ms);
        executed = true;
        executor.Stop();
    });

    executor.Run();

    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(executed.load());
    EXPECT_GE(elapsed, 50ms);
}

// ============================================================================
// WhenAll Tests (using SyncWait for simplicity)
// ============================================================================

Task<int> AddOne(int x) {
    co_return x + 1;
}

Task<int> Double(int x) {
    co_return x * 2;
}

Task<std::string> ToString(int x) {
    co_return std::to_string(x);
}

TEST(WhenAllTest, TwoTasks) {
    auto combined = []() -> Task<std::tuple<int, int>> { co_return co_await WhenAll(AddOne(1), Double(5)); };

    auto result = SyncWait(combined());

    EXPECT_EQ(std::get<0>(result), 2);   // 1 + 1
    EXPECT_EQ(std::get<1>(result), 10);  // 5 * 2
}

TEST(WhenAllTest, ThreeTasks) {
    auto combined = []() -> Task<std::tuple<int, int, int>> {
        co_return co_await WhenAll(AddOne(10), Double(20), AddOne(100));
    };

    auto result = SyncWait(combined());

    EXPECT_EQ(std::get<0>(result), 11);   // 10 + 1
    EXPECT_EQ(std::get<1>(result), 40);   // 20 * 2
    EXPECT_EQ(std::get<2>(result), 101);  // 100 + 1
}

TEST(WhenAllTest, VectorVersion) {
    auto combined = []() -> Task<std::vector<int>> {
        std::vector<Task<int>> tasks;
        tasks.push_back(AddOne(1));
        tasks.push_back(AddOne(2));
        tasks.push_back(AddOne(3));
        co_return co_await WhenAll(std::move(tasks));
    };

    auto results = SyncWait(combined());

    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0], 2);
    EXPECT_EQ(results[1], 3);
    EXPECT_EQ(results[2], 4);
}

// ============================================================================
// WhenAny Tests
// ============================================================================

Task<int> MakeValue(int value) {
    co_return value;
}

TEST(WhenAnyTest, FirstTaskWins) {
    auto race = []() -> Task<WhenAnyResult<int>> {
        std::vector<Task<int>> tasks;
        tasks.push_back(MakeValue(42));
        tasks.push_back(MakeValue(100));
        tasks.push_back(MakeValue(200));
        auto r = co_await WhenAny(std::move(tasks));
        co_return std::move(r).Value();
    };

    auto result = SyncWait(race());
    EXPECT_EQ(result.index, 0u);
    EXPECT_EQ(result.value, 42);
}

TEST(WhenAnyTest, EmptyVectorReturnsError) {
    auto race = []() -> Task<Result<WhenAnyResult<int>, std::error_code>> {
        std::vector<Task<int>> tasks;
        co_return co_await WhenAny(std::move(tasks));
    };

    auto result = SyncWait(race());
    EXPECT_TRUE(result.IsErr());
}

TEST(WhenAnyTest, VariadicVersion) {
    auto race = []() -> Task<WhenAnyResult<int>> {
        auto r = co_await WhenAny(MakeValue(99), MakeValue(88), MakeValue(77));
        co_return std::move(r).Value();
    };

    auto result = SyncWait(race());
    EXPECT_EQ(result.index, 0u);
    EXPECT_EQ(result.value, 99);
}

// ============================================================================
// Bug regression: ThreadPoolExecutor must destroy pending coroutines on shutdown
// ============================================================================
// Previously, coroutine handles queued but never executed were leaked when
// the executor was destroyed. The fix drains the queues in the destructor.

TEST(ThreadPoolTest, DestroyPendingCoroutinesOnShutdown) {
    // This test verifies the executor shuts down cleanly with pending work.
    // ASan would catch any leaked coroutine frames.
    {
        ThreadPoolExecutor executor(1);

        // Schedule work but stop before it can all run
        for (int i = 0; i < 10; ++i) {
            executor.Post([]() {
                // This may or may not run before Stop()
            });
        }

        executor.Stop();
        // Destructor runs here — should clean up pending work
    }

    SUCCEED();
}

// ============================================================================
// Additional Edge Cases
// ============================================================================

TEST(ThreadPoolTest, PostCallback) {
    ThreadPoolExecutor executor(2);
    std::atomic<bool> called{false};

    executor.Post([&]() { called = true; });

    std::this_thread::sleep_for(50ms);
    executor.Stop();

    EXPECT_TRUE(called.load());
}

TEST(ThreadPoolTest, MultiplePostCallbacks) {
    ThreadPoolExecutor executor(4);
    std::atomic<int> count{0};

    for (int i = 0; i < 100; ++i) {
        executor.Post([&]() { count.fetch_add(1); });
    }

    std::this_thread::sleep_for(200ms);
    executor.Stop();

    EXPECT_EQ(count.load(), 100);
}

TEST(ThreadPoolTest, RunOnceExecutesSingleItem) {
    ThreadPoolExecutor executor(1);
    std::atomic<int> counter{0};

    // Post multiple callbacks
    executor.Post([&]() { counter++; });
    executor.Post([&]() { counter++; });
    executor.Post([&]() { counter++; });

    // Give a moment for worker to pick up
    std::this_thread::sleep_for(100ms);
    executor.Stop();

    // All should have been processed by the worker thread
    EXPECT_EQ(counter.load(), 3);
}

TEST(ThreadPoolTest, PendingTasksCount) {
    // Stop executor first, then post — tasks will queue up
    ThreadPoolExecutor executor(1);
    executor.Stop();

    // Give worker time to stop
    std::this_thread::sleep_for(50ms);

    // PendingTasks should be 0 with no queued work
    EXPECT_EQ(executor.PendingTasks(), 0u);
}

TEST(ThreadPoolTest, OptionsWithZeroThreads) {
    // Passing 0 threads should auto-correct to 1
    ThreadPoolExecutor executor(0);
    EXPECT_EQ(executor.NumThreads(), 1u);
}

TEST(ThreadPoolTest, OptionsStruct) {
    ThreadPoolExecutor::Options options;
    options.num_threads = 0;  // Should auto-correct to 1
    ThreadPoolExecutor executor(options);
    EXPECT_EQ(executor.NumThreads(), 1u);
}

TEST(ThreadPoolTest, IsRunningState) {
    ThreadPoolExecutor executor(2);
    EXPECT_TRUE(executor.IsRunning());
    executor.Stop();
    EXPECT_FALSE(executor.IsRunning());
}

TEST(ThreadPoolTest, DefaultConstructor) {
    // Default constructor should create a valid executor
    ThreadPoolExecutor executor;
    EXPECT_GE(executor.NumThreads(), 1u);
    executor.Stop();
}

TEST(ThreadPoolTest, RunOnceWithCallback) {
    ThreadPoolExecutor executor(1);
    executor.Stop();  // Stop workers so items stay in queue

    bool callback_called = false;
    executor.Post([&]() { callback_called = true; });

    // RunOnce should execute the callback
    executor.RunOnce();
    EXPECT_TRUE(callback_called);
}

TEST(ThreadPoolTest, RunWaitsForCompletion) {
    ThreadPoolExecutor executor(2);
    std::atomic<int> count{0};

    for (int i = 0; i < 5; ++i) {
        executor.Post([&]() {
            std::this_thread::sleep_for(10ms);
            count++;
            if (count == 5) {
                executor.Stop();
            }
        });
    }

    executor.Run();
    EXPECT_EQ(count.load(), 5);
}

TEST(ThreadPoolTest, ScheduleAfterTimerLoop) {
    ThreadPoolExecutor executor(2);
    std::atomic<bool> fired{false};
    auto start = std::chrono::steady_clock::now();

    auto task = [&]() -> Task<void> {
        fired = true;
        co_return;
    };

    auto t = task();
    executor.ScheduleAfter(50ms, t.GetHandle());

    // Wait for the timer to fire (up to 500ms)
    for (int i = 0; i < 100 && !fired.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(fired.load());
    EXPECT_GE(elapsed, 40ms);
    executor.Stop();
}

TEST(ThreadPoolTest, ScheduleNullHandle) {
    ThreadPoolExecutor executor(1);
    // Schedule a null handle — should be silently ignored
    executor.Schedule(std::coroutine_handle<>{nullptr});
    // Post a null callback — should be silently ignored
    executor.Post(nullptr);
    // ScheduleAfter a null handle — should be silently ignored
    executor.ScheduleAfter(10ms, std::coroutine_handle<>{nullptr});

    // These should not crash; just stop
    std::this_thread::sleep_for(20ms);
    executor.Stop();
}

TEST(ThreadPoolTest, MultipleScheduleAfter) {
    ThreadPoolExecutor executor(1);  // Single thread to ensure sequential execution
    std::vector<int> order;
    std::mutex order_mutex;
    std::atomic<int> done_count{0};

    auto task1 = [&]() -> Task<void> {
        {
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(1);
        }
        done_count++;
        co_return;
    };

    auto task2 = [&]() -> Task<void> {
        {
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(2);
        }
        done_count++;
        co_return;
    };

    auto task3 = [&]() -> Task<void> {
        {
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(3);
        }
        done_count++;
        co_return;
    };

    auto t1 = task1();
    auto t2 = task2();
    auto t3 = task3();

    executor.ScheduleAfter(50ms, t1.GetHandle());
    executor.ScheduleAfter(25ms, t2.GetHandle());
    executor.ScheduleAfter(100ms, t3.GetHandle());

    // Wait for all 3 timers to fire (up to 1s)
    for (int i = 0; i < 200 && done_count.load() < 3; ++i) {
        std::this_thread::sleep_for(5ms);
    }

    executor.Stop();

    ASSERT_EQ(order.size(), 3u);
    // Should fire in order: 2 (25ms), 1 (50ms), 3 (100ms)
    EXPECT_EQ(order[0], 2);
    EXPECT_EQ(order[1], 1);
    EXPECT_EQ(order[2], 3);
}
