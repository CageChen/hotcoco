// ============================================================================
// ScheduleOn / Yield Tests
// ============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "hotcoco/core/schedule_on.hpp"
#include "hotcoco/core/spawn.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/io/libuv_executor.hpp"
#include "hotcoco/io/timer.hpp"
#include "hotcoco/sync/sync_wait.hpp"

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// Yield Tests
// ============================================================================

TEST(YieldTest, YieldWithoutExecutor) {
    // Without an executor, Yield degenerates to immediate resume
    auto task = []() -> Task<int> {
        co_await Yield();
        co_return 42;
    };

    int result = SyncWait(task());
    EXPECT_EQ(result, 42);
}

TEST(YieldTest, YieldWithExecutor) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    std::vector<int> order;

    // Two tasks: task1 yields after recording step 1, letting task2 run
    auto task1 = [&]() -> Task<void> {
        order.push_back(1);
        co_await Yield();
        order.push_back(3);
    };

    auto task2 = [&]() -> Task<void> {
        order.push_back(2);
        co_await Yield();
        order.push_back(4);
        executor.Stop();
    };

    Spawn(executor, task1());
    Spawn(executor, task2());
    executor.Run();

    ASSERT_EQ(order.size(), 4u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
    EXPECT_EQ(order[3], 4);
}

TEST(YieldTest, MultipleYields) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    int counter = 0;

    auto task = [&]() -> Task<void> {
        counter++;        // 1
        co_await Yield();
        counter++;        // 2
        co_await Yield();
        counter++;        // 3
        executor.Stop();
    };

    Spawn(executor, task());
    executor.Run();

    EXPECT_EQ(counter, 3);
}

// ============================================================================
// ScheduleOn Tests
// ============================================================================

TEST(ScheduleOnTest, TransferToSameExecutor) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    bool completed = false;

    auto task = [&]() -> Task<void> {
        co_await ScheduleOn(executor);
        completed = true;
        executor.Stop();
    };

    Spawn(executor, task());
    executor.Run();

    EXPECT_TRUE(completed);
}

TEST(ScheduleOnTest, TransferBetweenExecutors) {
    auto executor1_ptr = LibuvExecutor::Create().Value();
    auto& executor1 = *executor1_ptr;
    auto executor2_ptr = LibuvExecutor::Create().Value();
    auto& executor2 = *executor2_ptr;
    std::atomic<bool> completed{false};

    std::thread::id thread1_id;
    std::thread::id thread2_id;

    auto task = [&]() -> Task<void> {
        thread1_id = std::this_thread::get_id();

        // Transfer to executor2
        co_await ScheduleOn(executor2);

        thread2_id = std::this_thread::get_id();
        completed.store(true);
        executor2.Stop();
    };

    Spawn(executor1, task());

    // Run executor2 on a separate thread
    std::thread t2([&]() {
        executor2.Run();
    });

    // Run executor1 on this thread — it will start the task,
    // which transfers to executor2
    // Use a driver to keep executor1 alive briefly
    auto driver = [&]() -> Task<void> {
        co_await AsyncSleep(200ms);
        executor1.Stop();
    };
    Spawn(executor1, driver());
    executor1.Run();

    t2.join();

    EXPECT_TRUE(completed.load());
    // Verify the coroutine actually ran on different threads
    EXPECT_NE(thread1_id, thread2_id);
}

TEST(ScheduleOnTest, RoundTrip) {
    // Transfer to executor2 and back to executor1
    auto executor1_ptr = LibuvExecutor::Create().Value();
    auto& executor1 = *executor1_ptr;
    auto executor2_ptr = LibuvExecutor::Create().Value();
    auto& executor2 = *executor2_ptr;
    std::atomic<bool> completed{false};

    std::thread::id id_before;
    std::thread::id id_during;
    std::thread::id id_after;

    auto task = [&]() -> Task<void> {
        id_before = std::this_thread::get_id();

        co_await ScheduleOn(executor2);
        id_during = std::this_thread::get_id();

        co_await ScheduleOn(executor1);
        id_after = std::this_thread::get_id();

        completed.store(true);
        executor1.Stop();
    };

    Spawn(executor1, task());

    std::thread t2([&]() {
        executor2.Run();
    });

    // Keep executor2 alive with a driver
    auto driver2 = [&]() -> Task<void> {
        co_await AsyncSleep(500ms);
        executor2.Stop();
    };
    Spawn(executor2, driver2());

    executor1.Run();
    t2.join();

    EXPECT_TRUE(completed.load());
    // Before and after should be on the same thread (executor1)
    EXPECT_EQ(id_before, id_after);
    // During should be on a different thread (executor2)
    EXPECT_NE(id_before, id_during);
}

// ============================================================================
// Concept validation
// ============================================================================

#include "hotcoco/core/concepts.hpp"

TEST(ScheduleOnTest, YieldIsAwaitable) {
    static_assert(Awaitable<YieldAwaitable>);
    static_assert(AwaitableOfVoid<YieldAwaitable>);
}

TEST(ScheduleOnTest, ScheduleOnIsAwaitable) {
    static_assert(Awaitable<ScheduleOnAwaitable>);
    static_assert(AwaitableOfVoid<ScheduleOnAwaitable>);
}
