// ============================================================================
// hotcoco/core/timeout.hpp - Task Timeouts
// ============================================================================
//
// WithTimeout() wraps a task with a timeout, returning an error if the task
// doesn't complete within the specified duration.
//
// IMPLEMENTATION:
// ---------------
// Uses a race pattern: two child tasks (user task + timer) compete via
// atomic CAS. The first to complete sets an event; the loser silently
// finishes. All shared state is heap-allocated via shared_ptr so the
// DetachedTask controller can safely outlive the WithTimeout caller.
//
// Without an executor, the timer cannot fire, so the user's task always
// wins. This is correct behavior: in a pure cooperative context without
// an event loop, there is no mechanism for preemptive timeout.
//
// USAGE:
// ------
//   // Inside an executor context:
//   auto result = co_await WithTimeout(FetchData(), 5s);
//   if (!result) {
//       std::cerr << "Operation timed out!" << std::endl;
//   }
//
// ============================================================================

#pragma once

#include "hotcoco/core/detached_task.hpp"
#include "hotcoco/core/result.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/core/when_all.hpp"
#include "hotcoco/io/timer.hpp"
#include "hotcoco/sync/event.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace hotcoco {

// Timeout error type
struct TimeoutError {
    std::chrono::milliseconds duration;

    std::string Message() const { return "Operation timed out after " + std::to_string(duration.count()) + "ms"; }
};

namespace detail {

// Shared state for WithTimeout race — heap-allocated so both the caller
// and the DetachedTask controller (which may outlive the caller) can
// safely access it.
template <typename T>
struct TimeoutState {
    AsyncEvent notify;
    std::atomic<bool> first_completed{false};
    std::optional<T> user_result;
    bool timed_out = false;
};

struct TimeoutStateVoid {
    AsyncEvent notify;
    std::atomic<bool> first_completed{false};
    bool timed_out = false;
};

// Helper coroutines that take shared_ptr by value, ensuring the state
// outlives them even if the WithTimeout caller returns early.
// The Task<T> parameter is passed by value — it's moved onto the
// coroutine frame (heap-allocated), so it's safe across suspensions.

template <typename T>
Task<void> TimeoutUserChild(std::shared_ptr<TimeoutState<T>> state, Task<T> user_task) {
    T value = co_await std::move(user_task);
    bool expected = false;
    if (state->first_completed.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed)) {
        state->user_result.emplace(std::move(value));
        state->notify.Set();
    }
}

template <typename T, typename Rep, typename Period>
Task<void> TimeoutTimerChild(std::shared_ptr<TimeoutState<T>> state, std::chrono::duration<Rep, Period> timeout) {
    co_await AsyncSleep(timeout);
    bool expected = false;
    if (state->first_completed.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed)) {
        state->timed_out = true;
        state->notify.Set();
    }
}

inline Task<void> TimeoutUserChildVoid(std::shared_ptr<TimeoutStateVoid> state, Task<void> user_task) {
    co_await std::move(user_task);
    bool expected = false;
    if (state->first_completed.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed)) {
        state->notify.Set();
    }
}

template <typename Rep, typename Period>
Task<void> TimeoutTimerChildVoid(std::shared_ptr<TimeoutStateVoid> state, std::chrono::duration<Rep, Period> timeout) {
    co_await AsyncSleep(timeout);
    bool expected = false;
    if (state->first_completed.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed)) {
        state->timed_out = true;
        state->notify.Set();
    }
}

// Controller coroutine: owns all children, waits for ALL to complete,
// then self-destructs via DetachedTask.
template <typename T, typename Rep, typename Period>
Task<void> TimeoutController(std::shared_ptr<TimeoutState<T>> state, Task<T> user_task,
                             std::chrono::duration<Rep, Period> timeout) {
    std::vector<Task<void>> children;
    children.push_back(TimeoutUserChild<T>(state, std::move(user_task)));
    children.push_back(TimeoutTimerChild<T>(state, timeout));
    co_await WhenAll(std::move(children));
}

template <typename Rep, typename Period>
Task<void> TimeoutControllerVoid(std::shared_ptr<TimeoutStateVoid> state, Task<void> user_task,
                                 std::chrono::duration<Rep, Period> timeout) {
    std::vector<Task<void>> children;
    children.push_back(TimeoutUserChildVoid(state, std::move(user_task)));
    children.push_back(TimeoutTimerChildVoid(state, timeout));
    co_await WhenAll(std::move(children));
}

}  // namespace detail

// ============================================================================
// WithTimeout for Task<T> - returns Result<T, TimeoutError>
// ============================================================================
template <typename T, typename Rep, typename Period>
Task<Result<T, TimeoutError>> WithTimeout(Task<T> task, std::chrono::duration<Rep, Period> timeout) {
    auto state = std::make_shared<detail::TimeoutState<T>>();

    auto controller = MakeDetached(detail::TimeoutController<T>(state, std::move(task), timeout));
    controller.Start();

    co_await state->notify.Wait();

    if (state->timed_out) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
        co_return Err(TimeoutError{ms});
    }
    co_return Ok(std::move(state->user_result.value()));
}

// Specialization for Task<void>
template <typename Rep, typename Period>
Task<Result<void, TimeoutError>> WithTimeout(Task<void> task, std::chrono::duration<Rep, Period> timeout) {
    auto state = std::make_shared<detail::TimeoutStateVoid>();

    auto controller = MakeDetached(detail::TimeoutControllerVoid(state, std::move(task), timeout));
    controller.Start();

    co_await state->notify.Wait();

    if (state->timed_out) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
        co_return Err(TimeoutError{ms});
    }
    co_return Ok();
}

}  // namespace hotcoco
