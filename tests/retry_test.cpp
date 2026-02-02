// ============================================================================
// Retry Tests
// ============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "hotcoco/core/error.hpp"
#include "hotcoco/core/result.hpp"
#include "hotcoco/core/retry.hpp"
#include "hotcoco/core/spawn.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/io/libuv_executor.hpp"
#include "hotcoco/io/timer.hpp"
#include "hotcoco/sync/sync_wait.hpp"

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// Basic Retry Tests
// ============================================================================

TEST(RetryTest, SucceedsFirstTry) {
    auto test = []() -> Task<Result<int, std::error_code>> {
        co_return co_await Retry([]() -> Task<Result<int, std::error_code>> {
            co_return Ok(42);
        }).Execute();
    };

    auto result = SyncWait(test());
    ASSERT_TRUE(result.IsOk());
    EXPECT_EQ(result.Value(), 42);
}

TEST(RetryTest, SucceedsAfterRetry) {
    std::atomic<int> attempts{0};

    auto test = [&attempts]() -> Task<Result<int, std::error_code>> {
        co_return co_await Retry([&attempts]() -> Task<Result<int, std::error_code>> {
            if (++attempts < 3) {
                co_return Err(make_error_code(Errc::IoError));
            }
            co_return Ok(42);
        }).MaxAttempts(5)
          .WithExponentialBackoff(10ms)
          .Execute();
    };

    auto result = SyncWait(test());
    ASSERT_TRUE(result.IsOk());
    EXPECT_EQ(result.Value(), 42);
    EXPECT_EQ(attempts.load(), 3);
}

TEST(RetryTest, ExhaustsRetries) {
    std::atomic<int> attempts{0};

    auto test = [&attempts]() -> Task<Result<int, std::error_code>> {
        co_return co_await Retry([&attempts]() -> Task<Result<int, std::error_code>> {
            attempts++;
            co_return Err(make_error_code(Errc::IoError));
        }).MaxAttempts(3)
          .WithExponentialBackoff(1ms)
          .Execute();
    };

    auto result = SyncWait(test());
    EXPECT_TRUE(result.IsErr());
    EXPECT_EQ(result.Error(), make_error_code(Errc::IoError));
    EXPECT_EQ(attempts.load(), 3);
}

TEST(RetryTest, RetryIfPredicate) {
    std::atomic<int> attempts{0};

    auto test = [&attempts]() -> Task<Result<int, std::error_code>> {
        co_return co_await Retry([&attempts]() -> Task<Result<int, std::error_code>> {
            attempts++;
            co_return Err(make_error_code(Errc::InvalidArgument));
        }).MaxAttempts(5)
          .RetryIf([](std::error_code ec) {
              // Only retry IoError, not InvalidArgument
              return ec == make_error_code(Errc::IoError);
          })
          .Execute();
    };

    auto result = SyncWait(test());
    EXPECT_TRUE(result.IsErr());
    EXPECT_EQ(attempts.load(), 1);  // No retries for non-retryable error
}

TEST(RetryTest, NoJitter) {
    std::atomic<int> attempts{0};

    auto test = [&attempts]() -> Task<Result<int, std::error_code>> {
        co_return co_await Retry([&attempts]() -> Task<Result<int, std::error_code>> {
            if (++attempts < 2) {
                co_return Err(make_error_code(Errc::IoError));
            }
            co_return Ok(42);
        }).MaxAttempts(3)
          .WithExponentialBackoff(1ms)
          .NoJitter()
          .Execute();
    };

    auto result = SyncWait(test());
    ASSERT_TRUE(result.IsOk());
    EXPECT_EQ(result.Value(), 42);
    EXPECT_EQ(attempts.load(), 2);
}

// ============================================================================
// Bug regression: retry backoff must not block the executor thread
// ============================================================================

TEST(RetryTest, BackoffDoesNotBlockExecutor) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    std::atomic<bool> other_task_ran{false};
    std::atomic<int> attempts{0};

    auto retrying_task = [&]() -> Task<Result<int, std::error_code>> {
        co_return co_await Retry([&]() -> Task<Result<int, std::error_code>> {
            if (++attempts < 3) {
                co_return Err(make_error_code(Errc::IoError));
            }
            co_return Ok(42);
        }).MaxAttempts(5)
          .WithExponentialBackoff(50ms)
          .NoJitter()
          .Execute();
    };

    // This task should run DURING the retry backoff delays
    auto concurrent_task = [&]() -> Task<void> {
        co_await AsyncSleep(10ms);
        other_task_ran = true;
        co_return;
    };

    std::atomic<int> result{0};

    auto driver = [&]() -> Task<void> {
        Spawn(executor, retrying_task())
            .OnComplete([&](Result<int, std::error_code> v) {
                if (v.IsOk()) result = v.Value();
            });
        Spawn(executor, concurrent_task());

        co_await AsyncSleep(500ms);
        executor.Stop();
        co_return;
    };

    Spawn(executor, driver());
    executor.Run();

    EXPECT_EQ(result.load(), 42);
    EXPECT_EQ(attempts.load(), 3);
    EXPECT_TRUE(other_task_ran.load())
        << "Concurrent task did not run during retry backoff - "
           "executor thread was likely blocked by sleep_for";
}

// ============================================================================
// Additional Edge Cases
// ============================================================================

TEST(RetryTest, SingleAttemptSuccess) {
    auto test = []() -> Task<Result<int, std::error_code>> {
        co_return co_await Retry([]() -> Task<Result<int, std::error_code>> {
            co_return Ok(42);
        }).MaxAttempts(1).Execute();
    };

    auto result = SyncWait(test());
    ASSERT_TRUE(result.IsOk());
    EXPECT_EQ(result.Value(), 42);
}

TEST(RetryTest, SingleAttemptFails) {
    auto test = []() -> Task<Result<int, std::error_code>> {
        co_return co_await Retry([]() -> Task<Result<int, std::error_code>> {
            co_return Err(make_error_code(Errc::IoError));
        }).MaxAttempts(1).Execute();
    };

    auto result = SyncWait(test());
    EXPECT_TRUE(result.IsErr());
}

TEST(RetryTest, MaxDelayRespected) {
    std::atomic<int> attempts{0};

    auto test = [&]() -> Task<Result<int, std::error_code>> {
        co_return co_await Retry([&]() -> Task<Result<int, std::error_code>> {
            if (++attempts < 3) {
                co_return Err(make_error_code(Errc::IoError));
            }
            co_return Ok(42);
        }).MaxAttempts(5)
          .WithExponentialBackoff(1ms, 10.0)
          .MaxDelay(5ms)
          .NoJitter()
          .Execute();
    };

    auto start = std::chrono::steady_clock::now();
    auto result = SyncWait(test());
    auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(result.IsOk());
    EXPECT_EQ(result.Value(), 42);
    EXPECT_LT(elapsed, 500ms);
}
