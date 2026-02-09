// ============================================================================
// hotcoco/core/when_any.hpp - First-Completion Race
// ============================================================================
//
// WhenAny runs multiple tasks concurrently and returns as soon as ANY one
// completes. The result includes which task won (index) and its value.
//
// KEY CONCEPTS:
// -------------
// 1. THREE-LAYER ARCHITECTURE:
//    - WhenAny() function: owns AsyncEvent + optional<result> on its frame
//    - Controller (DetachedTask): owns all child tasks + atomic<bool>;
//      waits for ALL children via WhenAll, then self-destructs
//    - Child tasks (Task<void>): each wraps one user task; race via atomic CAS
//
// 2. ATOMIC CAS RACE: Each child uses compare_exchange_strong on a shared
//    atomic<bool> to determine the winner. Only the winner writes the result
//    and sets the event. Losers silently complete.
//
// 3. SAFE CLEANUP: The controller awaits ALL tasks (including losers) via
//    WhenAll before self-destructing, ensuring no memory leaks.
//
// USAGE:
// ------
//   auto result = co_await WhenAny(FastTask(), SlowTask());
//   std::cout << "Winner: task " << result.index << std::endl;
//
// ============================================================================

#pragma once

#include "hotcoco/core/detached_task.hpp"
#include "hotcoco/core/error.hpp"
#include "hotcoco/core/result.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/core/when_all.hpp"
#include "hotcoco/sync/event.hpp"

#include <atomic>
#include <coroutine>
#include <memory>
#include <optional>
#include <vector>

namespace hotcoco {

// ============================================================================
// WhenAnyResult - Result from WhenAny
// ============================================================================
template <typename T>
struct WhenAnyResult {
    size_t index;  // Which task completed first (0-based)
    T value;       // The result
};

template <>
struct WhenAnyResult<void> {
    size_t index;  // Which task completed first
};

// ============================================================================
// WhenAnySharedState - Shared state between WhenAny and its controller
// ============================================================================
// Prevents dangling references: after the winner signals the event, WhenAny
// resumes and may destroy its frame. The controller (still running, awaiting
// losers via WhenAll) would hold dangling references without shared ownership.
template <typename T>
struct WhenAnySharedState {
    AsyncEvent notify;
    std::optional<WhenAnyResult<T>> result;
    std::atomic<bool> first_completed{false};
};

namespace detail {

// ============================================================================
// Helper: make a child task that races via atomic CAS
// ============================================================================
template <typename T>
Task<void> MakeWhenAnyChild(Task<T> user_task, size_t index, std::shared_ptr<WhenAnySharedState<T>> state) {
    T value = co_await std::move(user_task);

    bool expected = false;
    if (state->first_completed.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed)) {
        // We won the race — store result and notify
        state->result.emplace(WhenAnyResult<T>{index, std::move(value)});
        state->notify.Set();
    }
}

// Void specialization
inline Task<void> MakeWhenAnyChildVoid(Task<void> user_task, size_t index,
                                       std::shared_ptr<WhenAnySharedState<void>> state) {
    co_await std::move(user_task);

    bool expected = false;
    if (state->first_completed.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed)) {
        state->result.emplace(WhenAnyResult<void>{index});
        state->notify.Set();
    }
}

// ============================================================================
// Controller: DetachedTask that owns all children and waits for ALL
// ============================================================================
template <typename T>
DetachedTask MakeWhenAnyController(std::vector<Task<T>> user_tasks, std::shared_ptr<WhenAnySharedState<T>> state) {
    std::vector<Task<void>> children;
    children.reserve(user_tasks.size());
    for (size_t i = 0; i < user_tasks.size(); ++i) {
        if constexpr (std::is_void_v<T>) {
            children.push_back(MakeWhenAnyChildVoid(std::move(user_tasks[i]), i, state));
        } else {
            children.push_back(MakeWhenAnyChild(std::move(user_tasks[i]), i, state));
        }
    }

    // Wait for ALL children (including losers) to complete
    co_await WhenAll(std::move(children));
}

}  // namespace detail

// ============================================================================
// WhenAny - Public API (Vector version)
// ============================================================================

template <typename T>
[[nodiscard]] Task<Result<WhenAnyResult<T>, std::error_code>> WhenAny(std::vector<Task<T>> tasks) {
    if (tasks.empty()) {
        co_return Err(make_error_code(Errc::InvalidArgument));
    }

    auto state = std::make_shared<WhenAnySharedState<T>>();

    auto controller = detail::MakeWhenAnyController<T>(std::move(tasks), state);
    controller.Start();  // Start the controller (which starts all children)

    co_await state->notify.Wait();  // Suspend until the first child wins

    co_return Ok(std::move(state->result.value()));
}

// ============================================================================
// WhenAny - Variadic version (homogeneous types)
// ============================================================================

template <typename T, typename... Rest>
    requires(std::same_as<T, Rest> && ...)
[[nodiscard]] Task<Result<WhenAnyResult<T>, std::error_code>> WhenAny(Task<T> first, Task<Rest>... rest) {
    std::vector<Task<T>> tasks;
    tasks.reserve(1 + sizeof...(Rest));
    tasks.push_back(std::move(first));
    (tasks.push_back(std::move(rest)), ...);
    co_return co_await WhenAny(std::move(tasks));
}

}  // namespace hotcoco
