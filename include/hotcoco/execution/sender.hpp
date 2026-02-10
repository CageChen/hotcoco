// ============================================================================
// hotcoco/execution/sender.hpp - Task<T> → P2300 Sender Adapter
// ============================================================================
//
// Wraps a hotcoco Task<T> as a P2300 sender, enabling composition with
// stdexec algorithms like then(), when_all(), etc.
//
// DESIGN:
// -------
// Task<T> is lazy and move-only. TaskSender<T> preserves both properties.
// On start(), we resume the task's coroutine handle directly. A bridge
// coroutine awaits the task and delivers the result to the receiver.
//
// Because hotcoco uses -fno-exceptions, we don't use set_error(exception_ptr).
// Errors are conveyed through Result<T> in the value channel.
//
// USAGE:
// ------
//   Task<int> ComputeAsync() { co_return 42; }
//
//   auto sender = hotcoco::execution::AsSender(ComputeAsync())
//               | stdexec::then([](int x) { return x * 2; });
//
// ============================================================================

#pragma once

#include "hotcoco/core/task.hpp"

#include <stdexec/execution.hpp>
#include <type_traits>
#include <utility>

namespace hotcoco::execution {

namespace detail {

// Bridge coroutine: awaits a Task<T> and delivers result to receiver.
// This is an eager (suspend_never initial_suspend) fire-and-forget coroutine
// that self-destructs on completion (suspend_never final_suspend).
template <typename T>
struct BridgeTask {
    struct promise_type {
        BridgeTask get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::abort(); }
    };
};

}  // namespace detail

// Forward declaration
template <typename T>
class TaskSender;

// ============================================================================
// TaskSenderOperation<T, Receiver> - operation_state
// ============================================================================
template <typename T, typename Receiver>
class TaskSenderOperation {
   public:
    TaskSenderOperation(Task<T> task, Receiver rcvr) noexcept : task_(std::move(task)), receiver_(std::move(rcvr)) {}

    TaskSenderOperation(TaskSenderOperation&&) = delete;
    TaskSenderOperation& operator=(TaskSenderOperation&&) = delete;

    void start() noexcept {
        // Launch bridge coroutine that awaits the task and delivers result.
        // Both task and receiver are passed by value so they live in the
        // coroutine frame, not via `this` which may be destroyed before
        // the bridge coroutine resumes from an async Task.
        run_bridge(std::move(task_), std::move(receiver_));
    }

   private:
    // Bridge coroutine: co_awaits the task, then completes the receiver.
    // All state is passed as parameters to live in the coroutine frame,
    // avoiding use-after-free when TaskSenderOperation is destroyed
    // before the bridge coroutine resumes.
    static auto run_bridge(Task<T> t, Receiver rcvr) -> detail::BridgeTask<T> {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(t);
            stdexec::set_value(std::move(rcvr));
        } else {
            auto result = co_await std::move(t);
            stdexec::set_value(std::move(rcvr), std::move(result));
        }
    }

    Task<T> task_;
    Receiver receiver_;
};

// ============================================================================
// TaskSender<T> - P2300 sender wrapping a Task<T>
// ============================================================================
template <typename T>
class TaskSender {
   public:
    using sender_concept = stdexec::sender_t;

    explicit TaskSender(Task<T> task) noexcept : task_(std::move(task)) {}

    // Move-only (Task is move-only)
    TaskSender(TaskSender&&) noexcept = default;
    TaskSender& operator=(TaskSender&&) noexcept = default;
    TaskSender(const TaskSender&) = delete;
    TaskSender& operator=(const TaskSender&) = delete;

    template <class Receiver>
    auto connect(Receiver rcvr) noexcept -> TaskSenderOperation<T, Receiver> {
        return TaskSenderOperation<T, Receiver>{std::move(task_), std::move(rcvr)};
    }

    template <class, class...>
    static consteval auto get_completion_signatures() noexcept {
        if constexpr (std::is_void_v<T>) {
            return stdexec::completion_signatures<stdexec::set_value_t()>{};
        } else {
            return stdexec::completion_signatures<stdexec::set_value_t(T)>{};
        }
    }

   private:
    Task<T> task_;
};

// Factory function
template <typename T>
TaskSender<T> AsSender(Task<T> task) noexcept {
    return TaskSender<T>{std::move(task)};
}

}  // namespace hotcoco::execution
