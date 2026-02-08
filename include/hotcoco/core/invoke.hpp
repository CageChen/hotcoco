// ============================================================================
// hotcoco/core/invoke.hpp - Safe Lambda Capture for Coroutines
// ============================================================================
//
// Invoke() copies a callable and its arguments onto a stable coroutine frame
// before invoking it, preventing use-after-free of captured references across
// suspension points.
//
// THE PROBLEM:
// ------------
//   int a = 1;
//   auto make_task = [&a]() -> Task<int> {
//       co_await SomethingThatSuspends();
//       return a;  // DANGER: 'a' may be destroyed after suspension!
//   };
//   co_await make_task();  // Undefined behavior
//
// THE SOLUTION:
// -------------
//   co_await Invoke(make_task);  // Safe: lambda is copied onto coroutine frame
//
// HOW IT WORKS:
// -------------
// 1. MakeInvokerTask() is a coroutine that takes the functor BY VALUE,
//    copying it onto its coroutine frame. It then calls the functor to
//    produce a Task<T> and co_awaits it.
// 2. Invoke() is a regular function (not a coroutine) that creates the
//    invoker task and returns it. The task remains lazy — it executes
//    when co_awaited, just like any other Task<T>.
//
// ============================================================================

#pragma once

#include "hotcoco/core/task.hpp"

#include <utility>

namespace hotcoco {

namespace detail {

// Coroutine that takes the functor BY VALUE (copied onto the frame),
// invokes it with the given args, and co_awaits the resulting task.
template <typename F, typename... Args>
auto MakeInvokerTask(F functor, Args&&... args) -> decltype(functor(std::forward<Args>(args)...)) {
    auto user_task = functor(std::forward<Args>(args)...);
    co_return co_await user_task;
}

}  // namespace detail

// ============================================================================
// Invoke - Safe coroutine invocation with stable captures
// ============================================================================
//
// Takes a functor by value (copying it onto a coroutine frame) and forwards
// arguments to it. Returns the resulting lazy task.
//
// NOTE: Unlike libcoro's invoke() which eagerly resumes past initial_suspend,
// hotcoco's Task::Awaiter::await_ready() always returns false (no done-check),
// so we keep the task lazy. The co_await chain handles execution correctly.
//
template <typename F, typename... Args>
auto Invoke(F functor, Args&&... args) -> decltype(auto) {
    return detail::MakeInvokerTask(std::forward<F>(functor), std::forward<Args>(args)...);
}

}  // namespace hotcoco
