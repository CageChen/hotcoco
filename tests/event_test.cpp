// ============================================================================
// AsyncEvent Tests
// ============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "hotcoco/core/task.hpp"
#include "hotcoco/sync/event.hpp"
#include "hotcoco/sync/sync_wait.hpp"

using namespace hotcoco;

// ============================================================================
// Basic functionality
// ============================================================================

TEST(AsyncEventTest, InitiallyNotSet) {
    AsyncEvent event;
    EXPECT_FALSE(event.IsSet());
}

TEST(AsyncEventTest, SetMakesItSet) {
    AsyncEvent event;
    event.Set();
    EXPECT_TRUE(event.IsSet());
}

TEST(AsyncEventTest, ResetClearsIt) {
    AsyncEvent event;
    event.Set();
    EXPECT_TRUE(event.IsSet());
    event.Reset();
    EXPECT_FALSE(event.IsSet());
}

TEST(AsyncEventTest, DoubleSetIsIdempotent) {
    AsyncEvent event;
    event.Set();
    event.Set();
    EXPECT_TRUE(event.IsSet());
}

// ============================================================================
// co_await behavior
// ============================================================================

TEST(AsyncEventTest, WaitOnAlreadySetPassesThrough) {
    AsyncEvent event;
    event.Set();

    auto task = [&]() -> Task<int> {
        co_await event.Wait();
        co_return 42;
    };

    int result = SyncWait(task());
    EXPECT_EQ(result, 42);
}

TEST(AsyncEventTest, WaitSuspendsUntilSet) {
    AsyncEvent event;
    std::atomic<int> order{0};

    auto waiter = [&]() -> Task<void> {
        order.store(1);
        co_await event.Wait();
        order.store(3);
    };

    auto setter = [&]() -> Task<void> {
        order.store(2);
        event.Set();
        co_return;
    };

    // The waiter should suspend, then the setter resumes it
    auto combined = [&]() -> Task<void> {
        // Start waiter - it will suspend at co_await event.Wait()
        auto w = waiter();
        auto s = setter();

        // Manually drive: resume waiter (it suspends at event.Wait)
        w.GetHandle().resume();
        EXPECT_EQ(order.load(), 1);

        // Resume setter (it calls event.Set which resumes waiter)
        s.GetHandle().resume();
        EXPECT_EQ(order.load(), 3);

        co_return;
    };

    SyncWait(combined());
}

TEST(AsyncEventTest, MultipleWaitersAllResumed) {
    AsyncEvent event;
    std::atomic<int> count{0};

    auto make_waiter = [&]() -> Task<void> {
        co_await event.Wait();
        count.fetch_add(1);
    };

    auto orchestrator = [&]() -> Task<void> {
        auto w1 = make_waiter();
        auto w2 = make_waiter();
        auto w3 = make_waiter();

        // Start all three - they all suspend at event.Wait()
        w1.GetHandle().resume();
        w2.GetHandle().resume();
        w3.GetHandle().resume();
        EXPECT_EQ(count.load(), 0);

        // Set the event - all three should be resumed
        event.Set();
        EXPECT_EQ(count.load(), 3);

        co_return;
    };

    SyncWait(orchestrator());
}

// ============================================================================
// Reset and reuse
// ============================================================================

TEST(AsyncEventTest, ResetAndReuse) {
    AsyncEvent event;
    std::atomic<int> count{0};

    auto make_waiter = [&]() -> Task<void> {
        co_await event.Wait();
        count.fetch_add(1);
    };

    auto orchestrator = [&]() -> Task<void> {
        // First round
        auto w1 = make_waiter();
        w1.GetHandle().resume();
        EXPECT_EQ(count.load(), 0);
        event.Set();
        EXPECT_EQ(count.load(), 1);

        // Reset for second round
        event.Reset();

        auto w2 = make_waiter();
        w2.GetHandle().resume();
        EXPECT_EQ(count.load(), 1);  // Still 1, waiting
        event.Set();
        EXPECT_EQ(count.load(), 2);

        co_return;
    };

    SyncWait(orchestrator());
}

// ============================================================================
// Thread safety
// ============================================================================

TEST(AsyncEventTest, CrossThreadSetAndWait) {
    AsyncEvent event;
    std::atomic<bool> done{false};

    std::thread setter([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        event.Set();
    });

    auto waiter = [&]() -> Task<void> {
        co_await event.Wait();
        done.store(true);
    };

    // Start the waiter coroutine on this thread
    auto task = waiter();
    task.GetHandle().resume();  // Suspends at event.Wait()

    setter.join();
    EXPECT_TRUE(done.load());
}

// ============================================================================
// Additional Edge Cases
// ============================================================================

TEST(AsyncEventTest, DoubleReset) {
    AsyncEvent event;
    event.Reset();  // Already unset
    EXPECT_FALSE(event.IsSet());
    event.Reset();  // Still unset
    EXPECT_FALSE(event.IsSet());
}

TEST(AsyncEventTest, SetResetSet) {
    AsyncEvent event;
    event.Set();
    EXPECT_TRUE(event.IsSet());
    event.Reset();
    EXPECT_FALSE(event.IsSet());
    event.Set();
    EXPECT_TRUE(event.IsSet());
}

TEST(AsyncEventTest, WaitAfterSetImmediate) {
    AsyncEvent event;
    event.Set();

    // Multiple waits should all pass through immediately
    auto task = [&]() -> Task<int> {
        co_await event.Wait();
        co_await event.Wait();
        co_await event.Wait();
        co_return 42;
    };

    EXPECT_EQ(SyncWait(task()), 42);
}
