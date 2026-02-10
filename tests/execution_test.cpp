// ============================================================================
// execution_test.cpp - Tests for P2300 std::execution adapters
// ============================================================================

#ifdef HOTCOCO_HAS_STDEXEC

#include <atomic>
#include <condition_variable>
#include <gtest/gtest.h>
#include <hotcoco/core/cancellation.hpp>
#include <hotcoco/core/schedule_on.hpp>
#include <hotcoco/core/task.hpp>
#include <hotcoco/execution/as_awaitable.hpp>
#include <hotcoco/execution/scheduler.hpp>
#include <hotcoco/execution/sender.hpp>
#include <hotcoco/execution/stop_token_adapter.hpp>
#include <hotcoco/io/libuv_executor.hpp>
#include <hotcoco/sync/sync_wait.hpp>
#include <mutex>
#include <stdexec/execution.hpp>
#include <thread>

using namespace hotcoco;

// ============================================================================
// Scheduler Tests
// ============================================================================

TEST(ExecutionTest, SchedulerEquality) {
    auto exec = LibuvExecutor::Create().Value();
    auto s1 = execution::AsScheduler(*exec);
    auto s2 = execution::AsScheduler(*exec);
    EXPECT_EQ(s1, s2);
}

TEST(ExecutionTest, SchedulerInequality) {
    auto exec1 = LibuvExecutor::Create().Value();
    auto exec2 = LibuvExecutor::Create().Value();
    auto s1 = execution::AsScheduler(*exec1);
    auto s2 = execution::AsScheduler(*exec2);
    EXPECT_NE(s1, s2);
}

TEST(ExecutionTest, SchedulerScheduleAndSyncWait) {
    auto exec = LibuvExecutor::Create().Value();
    auto scheduler = execution::AsScheduler(*exec);

    // Run the executor in a background thread. Use a condvar to ensure
    // the executor loop is actually spinning before we post work.
    std::mutex mtx;
    std::condition_variable cv;
    bool running = false;

    std::thread executor_thread([&] {
        exec->Post([&] {
            std::lock_guard lk(mtx);
            running = true;
            cv.notify_one();
        });
        exec->Run();
    });

    {
        std::unique_lock lk(mtx);
        cv.wait(lk, [&] { return running; });
    }

    auto sender = stdexec::schedule(scheduler) | stdexec::then([] { return 42; });
    auto result = stdexec::sync_wait(std::move(sender));

    // Stop the executor from within its own loop to ensure uv_stop wakes it.
    exec->Post([&] { exec->Stop(); });
    executor_thread.join();

    ASSERT_TRUE(result.has_value());
    auto [value] = *result;
    EXPECT_EQ(value, 42);
}

TEST(ExecutionTest, SchedulerScheduleVoid) {
    auto exec = LibuvExecutor::Create().Value();
    auto scheduler = execution::AsScheduler(*exec);

    std::mutex mtx;
    std::condition_variable cv;
    bool loop_ready = false;

    std::thread executor_thread([&] {
        exec->Post([&] {
            std::lock_guard lk(mtx);
            loop_ready = true;
            cv.notify_one();
        });
        exec->Run();
    });

    {
        std::unique_lock lk(mtx);
        cv.wait(lk, [&] { return loop_ready; });
    }

    bool executed = false;
    auto sender = stdexec::schedule(scheduler) | stdexec::then([&] { executed = true; });
    stdexec::sync_wait(std::move(sender));

    exec->Post([&] { exec->Stop(); });
    executor_thread.join();

    EXPECT_TRUE(executed);
}

// ============================================================================
// Task → Sender Tests
// ============================================================================

TEST(ExecutionTest, TaskAsSender) {
    auto make_task = []() -> Task<int> { co_return 42; };

    auto sender = execution::AsSender(make_task());
    auto result = stdexec::sync_wait(std::move(sender));

    ASSERT_TRUE(result.has_value());
    auto [value] = *result;
    EXPECT_EQ(value, 42);
}

TEST(ExecutionTest, TaskAsSenderVoid) {
    bool executed = false;
    auto make_task = [&]() -> Task<void> {
        executed = true;
        co_return;
    };

    auto sender = execution::AsSender(make_task());
    auto result = stdexec::sync_wait(std::move(sender));

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(executed);
}

TEST(ExecutionTest, SenderThenChaining) {
    auto make_task = []() -> Task<int> { co_return 21; };

    auto sender = execution::AsSender(make_task()) | stdexec::then([](int x) { return x * 2; });
    auto result = stdexec::sync_wait(std::move(sender));

    ASSERT_TRUE(result.has_value());
    auto [value] = *result;
    EXPECT_EQ(value, 42);
}

TEST(ExecutionTest, TaskAsSenderMultipleChains) {
    auto make_task = []() -> Task<int> { co_return 10; };

    auto sender = execution::AsSender(make_task()) | stdexec::then([](int x) { return x + 5; }) |
                  stdexec::then([](int x) { return x * 2; });
    auto result = stdexec::sync_wait(std::move(sender));

    ASSERT_TRUE(result.has_value());
    auto [value] = *result;
    EXPECT_EQ(value, 30);
}

// ============================================================================
// Sender → co_await Tests
// ============================================================================

TEST(ExecutionTest, SenderAsAwaitable) {
    auto make_task = []() -> Task<int> {
        auto sender = stdexec::just(42);
        int result = co_await execution::AsAwaitable(std::move(sender));
        co_return result;
    };

    int result = SyncWait(make_task());
    EXPECT_EQ(result, 42);
}

TEST(ExecutionTest, SenderAsAwaitableWithThen) {
    auto make_task = []() -> Task<int> {
        auto sender = stdexec::just(21) | stdexec::then([](int x) { return x * 2; });
        int result = co_await execution::AsAwaitable(std::move(sender));
        co_return result;
    };

    int result = SyncWait(make_task());
    EXPECT_EQ(result, 42);
}

TEST(ExecutionTest, SenderAsAwaitableVoid) {
    bool executed = false;
    auto make_task = [&]() -> Task<void> {
        auto sender = stdexec::just();
        co_await execution::AsAwaitable(std::move(sender));
        executed = true;
        co_return;
    };

    SyncWait(make_task());
    EXPECT_TRUE(executed);
}

// ============================================================================
// stop_token ↔ CancellationToken Tests
// ============================================================================

TEST(ExecutionTest, StopTokenBridge) {
    std::stop_source ss;
    auto [token, bridge] = execution::FromStopToken(ss.get_token());

    EXPECT_FALSE(token.IsCancelled());

    ss.request_stop();

    EXPECT_TRUE(token.IsCancelled());
}

TEST(ExecutionTest, StopTokenBridgeAlreadyStopped) {
    std::stop_source ss;
    ss.request_stop();

    auto [token, bridge] = execution::FromStopToken(ss.get_token());

    EXPECT_TRUE(token.IsCancelled());
}

TEST(ExecutionTest, LinkCancellationToStopSource) {
    CancellationSource source;
    std::stop_source ss;

    {
        auto link = execution::LinkCancellation(source.GetToken(), ss);
        EXPECT_FALSE(ss.stop_requested());

        source.Cancel();
        EXPECT_TRUE(ss.stop_requested());
    }
}

TEST(ExecutionTest, LinkCancellationUnregistersOnDestruction) {
    CancellationSource source;
    std::stop_source ss;

    {
        auto link = execution::LinkCancellation(source.GetToken(), ss);
    }
    // Link destroyed — callback unregistered.
    // Cancelling now should not crash (callback no longer registered).
    source.Cancel();
    // ss was not stopped because the link was already destroyed
    EXPECT_FALSE(ss.stop_requested());
}

// ============================================================================
// Combined Tests
// ============================================================================

TEST(ExecutionTest, WhenAllSenders) {
    auto t1 = []() -> Task<int> { co_return 10; };
    auto t2 = []() -> Task<int> { co_return 20; };

    auto sender = stdexec::when_all(execution::AsSender(t1()), execution::AsSender(t2()));
    auto result = stdexec::sync_wait(std::move(sender));

    ASSERT_TRUE(result.has_value());
    auto [v1, v2] = *result;
    EXPECT_EQ(v1, 10);
    EXPECT_EQ(v2, 20);
}

// ============================================================================
// Async Task → Sender Tests (validates run_bridge UAF fix)
// ============================================================================

TEST(ExecutionTest, AsyncTaskAsSender) {
    // This test exercises the async path: the Task suspends on ScheduleOn
    // and resumes from the executor thread. Before the fix, run_bridge would
    // access receiver_ via a dangling `this` pointer after the operation_state
    // was destroyed.
    auto exec = LibuvExecutor::Create().Value();

    std::mutex mtx;
    std::condition_variable cv;
    bool loop_ready = false;

    std::thread executor_thread([&] {
        exec->Post([&] {
            std::lock_guard lk(mtx);
            loop_ready = true;
            cv.notify_one();
        });
        exec->Run();
    });

    {
        std::unique_lock lk(mtx);
        cv.wait(lk, [&] { return loop_ready; });
    }

    // Task that transfers to the executor — truly async
    auto make_async_task = [&exec]() -> Task<int> {
        co_await ScheduleOn(*exec);
        co_return 42;
    };

    auto sender = execution::AsSender(make_async_task());
    auto result = stdexec::sync_wait(std::move(sender));

    exec->Post([&] { exec->Stop(); });
    executor_thread.join();

    ASSERT_TRUE(result.has_value());
    auto [value] = *result;
    EXPECT_EQ(value, 42);
}

TEST(ExecutionTest, AsyncTaskAsSenderVoid) {
    auto exec = LibuvExecutor::Create().Value();

    std::mutex mtx;
    std::condition_variable cv;
    bool loop_ready = false;

    std::thread executor_thread([&] {
        exec->Post([&] {
            std::lock_guard lk(mtx);
            loop_ready = true;
            cv.notify_one();
        });
        exec->Run();
    });

    {
        std::unique_lock lk(mtx);
        cv.wait(lk, [&] { return loop_ready; });
    }

    bool executed = false;
    auto make_async_task = [&exec, &executed]() -> Task<void> {
        co_await ScheduleOn(*exec);
        executed = true;
        co_return;
    };

    auto sender = execution::AsSender(make_async_task());
    stdexec::sync_wait(std::move(sender));

    exec->Post([&] { exec->Stop(); });
    executor_thread.join();

    EXPECT_TRUE(executed);
}

// ============================================================================
// CancellationLink value-capture safety test
// ============================================================================

TEST(ExecutionTest, LinkCancellationSafeAfterSourceDestroyed) {
    // Verify that the callback lambda holds its own copy of std::stop_source
    // (value capture), so the link remains safe even if the caller's
    // stop_source is destroyed before Cancel() is called.
    CancellationSource source;
    auto link = [&]() {
        std::stop_source ss;  // will be destroyed at end of scope
        auto l = execution::LinkCancellation(source.GetToken(), ss);
        // ss is destroyed here, but the link should still be safe
        return std::make_pair(std::move(l), ss.get_token());
    }();

    // Cancel after the original stop_source is destroyed — this should
    // NOT crash even though the original stop_source no longer exists.
    source.Cancel();
    // The token from the returned pair should reflect the stop via the
    // value-captured copy inside the link.
    EXPECT_TRUE(link.second.stop_requested());
}

// ============================================================================
// AsAwaitable with scheduler-based sender
// ============================================================================

TEST(ExecutionTest, SenderAsAwaitableWithScheduler) {
    auto exec = LibuvExecutor::Create().Value();
    auto scheduler = execution::AsScheduler(*exec);

    std::mutex mtx;
    std::condition_variable cv;
    bool loop_ready = false;

    std::thread executor_thread([&] {
        exec->Post([&] {
            std::lock_guard lk(mtx);
            loop_ready = true;
            cv.notify_one();
        });
        exec->Run();
    });

    {
        std::unique_lock lk(mtx);
        cv.wait(lk, [&] { return loop_ready; });
    }

    auto make_task = [&scheduler]() -> Task<int> {
        auto sender = stdexec::schedule(scheduler) | stdexec::then([] { return 99; });
        int result = co_await execution::AsAwaitable(std::move(sender));
        co_return result;
    };

    int result = SyncWait(make_task());

    exec->Post([&] { exec->Stop(); });
    executor_thread.join();

    EXPECT_EQ(result, 99);
}

// ============================================================================
// Scheduler pipeline with let_stopped
// ============================================================================

TEST(ExecutionTest, SchedulerPipelineWithLetStopped) {
    // Verify that piping schedule() through let_stopped doesn't interfere
    // with the normal value path. (sync_wait owns its own stop_source, so
    // the set_stopped branch inside ScheduleOperation is unreachable here.)
    auto exec = LibuvExecutor::Create().Value();
    auto scheduler = execution::AsScheduler(*exec);

    std::mutex mtx;
    std::condition_variable cv;
    bool loop_ready = false;

    std::thread executor_thread([&] {
        exec->Post([&] {
            std::lock_guard lk(mtx);
            loop_ready = true;
            cv.notify_one();
        });
        exec->Run();
    });

    {
        std::unique_lock lk(mtx);
        cv.wait(lk, [&] { return loop_ready; });
    }

    auto sender = stdexec::schedule(scheduler)                         //
                  | stdexec::let_stopped([] {                          //
                        return stdexec::just(std::string("stopped"));  //
                    })                                                 //
                  | stdexec::then([](auto&&... args) -> std::string {
                        if constexpr (sizeof...(args) == 0) {
                            return "not_stopped";
                        } else {
                            return (..., std::string(args));
                        }
                    });

    auto result = stdexec::sync_wait(std::move(sender));
    ASSERT_TRUE(result.has_value());
    auto [value] = *result;
    EXPECT_EQ(value, "not_stopped");

    exec->Post([&] { exec->Stop(); });
    executor_thread.join();
}

// ============================================================================
// Sequential multiple AsAwaitable in one coroutine
// ============================================================================

TEST(ExecutionTest, SequentialAsAwaitableInOneCoroutine) {
    // Verify that a single coroutine can co_await multiple senders sequentially.
    // Each co_await creates a separate SenderAwaitable temporary in the coroutine frame.
    auto make_task = []() -> Task<int> {
        auto s1 = stdexec::just(10);
        int a = co_await execution::AsAwaitable(std::move(s1));

        auto s2 = stdexec::just(20) | stdexec::then([](int x) { return x + 5; });
        int b = co_await execution::AsAwaitable(std::move(s2));

        auto s3 = stdexec::just(a, b) | stdexec::then([](int x, int y) { return x + y; });
        int c = co_await execution::AsAwaitable(std::move(s3));

        co_return c;
    };

    int result = SyncWait(make_task());
    EXPECT_EQ(result, 35);  // 10 + (20 + 5) = 35
}

// ============================================================================
// AsAwaitable with composed sender (when_all)
// ============================================================================

TEST(ExecutionTest, AsAwaitableWithWhenAll) {
    // co_await a when_all sender — tests multi-value unpacking inside AsAwaitable.
    // when_all<just(int), just(int)> produces a tuple-like completion.
    // However, AsAwaitable extracts a single value type, so we pipe through
    // then() to reduce to a single int first.
    auto make_task = []() -> Task<int> {
        auto sender =
            stdexec::when_all(stdexec::just(10), stdexec::just(20)) | stdexec::then([](int a, int b) { return a + b; });
        int result = co_await execution::AsAwaitable(std::move(sender));
        co_return result;
    };

    int result = SyncWait(make_task());
    EXPECT_EQ(result, 30);
}

// ============================================================================
// FromStopToken with stop_impossible token
// ============================================================================

TEST(ExecutionTest, FromStopTokenImpossible) {
    // A default-constructed std::stop_token has stop_possible() == false.
    // FromStopToken should handle this gracefully without registering a callback.
    std::stop_token impossible_token;
    ASSERT_FALSE(impossible_token.stop_possible());

    auto [token, bridge] = execution::FromStopToken(impossible_token);
    // The token should not be cancelled (no one can signal it).
    EXPECT_FALSE(token.IsCancelled());
}

// ============================================================================
// when_all + async Tasks (cross-executor concurrency)
// ============================================================================

TEST(ExecutionTest, WhenAllAsyncTasks) {
    // Verify when_all works with async Tasks that actually suspend and
    // resume on an executor thread.
    auto exec = LibuvExecutor::Create().Value();

    std::mutex mtx;
    std::condition_variable cv;
    bool loop_ready = false;

    std::thread executor_thread([&] {
        exec->Post([&] {
            std::lock_guard lk(mtx);
            loop_ready = true;
            cv.notify_one();
        });
        exec->Run();
    });

    {
        std::unique_lock lk(mtx);
        cv.wait(lk, [&] { return loop_ready; });
    }

    auto make_task_a = [&exec]() -> Task<int> {
        co_await ScheduleOn(*exec);
        co_return 100;
    };

    auto make_task_b = [&exec]() -> Task<int> {
        co_await ScheduleOn(*exec);
        co_return 200;
    };

    auto sender = stdexec::when_all(execution::AsSender(make_task_a()), execution::AsSender(make_task_b()));
    auto result = stdexec::sync_wait(std::move(sender));

    exec->Post([&] { exec->Stop(); });
    executor_thread.join();

    ASSERT_TRUE(result.has_value());
    auto [a, b] = *result;
    EXPECT_EQ(a, 100);
    EXPECT_EQ(b, 200);
}

// ============================================================================
// TaskSender move semantics
// ============================================================================

TEST(ExecutionTest, TaskSenderMoveSemantics) {
    // Verify that TaskSender is move-only and move-constructible.
    using sender_t = decltype(execution::AsSender(std::declval<Task<int>>()));
    static_assert(std::is_move_constructible_v<sender_t>);
    static_assert(!std::is_copy_constructible_v<sender_t>);

    auto task = []() -> Task<int> { co_return 42; }();

    auto sender1 = execution::AsSender(std::move(task));
    auto sender2 = std::move(sender1);

    auto result = stdexec::sync_wait(std::move(sender2));
    ASSERT_TRUE(result.has_value());
    auto [value] = *result;
    EXPECT_EQ(value, 42);
}

#endif  // HOTCOCO_HAS_STDEXEC
