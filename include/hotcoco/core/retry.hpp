// ============================================================================
// hotcoco/core/retry.hpp - Retry with Exponential Backoff
// ============================================================================
//
// Retry provides automatic retry logic for failing operations with
// configurable backoff strategies.
//
// USAGE:
// ------
//   auto result = co_await Retry(FetchData())
//       .MaxAttempts(3)
//       .WithExponentialBackoff(100ms, 2.0)
//       .Execute();
//
// ============================================================================

#pragma once

#include "hotcoco/core/error.hpp"
#include "hotcoco/core/result.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/io/timer.hpp"

#include <chrono>
#include <functional>
#include <random>
#include <system_error>

namespace hotcoco {

// ============================================================================
// RetryPolicy - Configuration for retry behavior
// ============================================================================
struct RetryPolicy {
    size_t max_attempts = 3;
    std::chrono::milliseconds initial_delay = std::chrono::milliseconds(100);
    double backoff_multiplier = 2.0;
    std::chrono::milliseconds max_delay = std::chrono::milliseconds(30000);
    bool add_jitter = true;

    // Predicate to check if error is retryable
    std::function<bool(std::error_code)> should_retry = [](std::error_code) { return true; };
};

// ============================================================================
// RetryBuilder - Fluent builder for retry operations
// ============================================================================
//
// The task factory must return Task<Result<T, std::error_code>>.
// On success, the Result is forwarded. On error, the operation is retried
// according to the policy.
//
template <typename T>
class RetryBuilder {
   public:
    using TaskFactory = std::function<Task<Result<T, std::error_code>>()>;

    explicit RetryBuilder(TaskFactory factory) : factory_(std::move(factory)) {}

    RetryBuilder& MaxAttempts(size_t n) {
        policy_.max_attempts = n;
        return *this;
    }

    RetryBuilder& WithExponentialBackoff(std::chrono::milliseconds initial, double multiplier = 2.0) {
        policy_.initial_delay = initial;
        policy_.backoff_multiplier = multiplier;
        return *this;
    }

    RetryBuilder& MaxDelay(std::chrono::milliseconds max) {
        policy_.max_delay = max;
        return *this;
    }

    RetryBuilder& NoJitter() {
        policy_.add_jitter = false;
        return *this;
    }

    RetryBuilder& RetryIf(std::function<bool(std::error_code)> pred) {
        policy_.should_retry = std::move(pred);
        return *this;
    }

    // Execute with retry — returns Result
    Task<Result<T, std::error_code>> Execute() {
        std::error_code last_error;
        auto delay = policy_.initial_delay;

        for (size_t attempt = 0; attempt < policy_.max_attempts; ++attempt) {
            auto result = co_await factory_();
            if (result.IsOk()) {
                co_return std::move(result);
            }

            last_error = result.Error();

            bool should = (attempt + 1 < policy_.max_attempts) && policy_.should_retry(last_error);
            if (!should) {
                break;
            }

            // Calculate delay with optional jitter
            auto actual_delay = delay;
            if (policy_.add_jitter) {
                static thread_local std::mt19937 rng{std::random_device{}()};
                std::uniform_real_distribution<> dist(0.5, 1.5);
                actual_delay = std::chrono::milliseconds(static_cast<long>(delay.count() * dist(rng)));
            }

            // Cap at max delay
            if (actual_delay > policy_.max_delay) {
                actual_delay = policy_.max_delay;
            }

            // Wait before retry (async - doesn't block the executor)
            co_await AsyncSleep(actual_delay);

            // Increase delay for next attempt
            delay = std::chrono::milliseconds(static_cast<long>(delay.count() * policy_.backoff_multiplier));
        }

        // All attempts failed
        co_return Err(last_error ? last_error : make_error_code(Errc::RetryExhausted));
    }

   private:
    TaskFactory factory_;
    RetryPolicy policy_;
};

// ============================================================================
// Retry helper function
// ============================================================================

// Helper to extract T from Task<Result<T, E>>
template <typename T>
struct RetryResultType;

template <typename T, typename E>
struct RetryResultType<Task<Result<T, E>>> {
    using type = T;
};

template <typename F>
auto Retry(F&& factory) {
    using TaskType = std::invoke_result_t<F>;
    using ReturnType = typename RetryResultType<TaskType>::type;
    return RetryBuilder<ReturnType>([f = std::forward<F>(factory)]() mutable { return f(); });
}

}  // namespace hotcoco
