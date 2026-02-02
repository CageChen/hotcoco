// ============================================================================
// hotcoco/core/combinators.hpp - Coroutine Combinators
// ============================================================================
//
// This header provides combinators for composing multiple coroutines:
//
// - Then: Chain a continuation onto a task (promise chaining)
//
// KEY CONCEPTS:
// -------------
// PROMISE CHAINING: Attach a callback to transform a task's result.
// Similar to .then() in JavaScript Promises or .map() in Rust futures.
//
// USAGE:
// ------
//   Task<int> doubled = Then(GetValue(), [](int x) { return x * 2; });
//
// NOTE ON WHEN_ALL/WHEN_ANY:
// WhenAll and WhenAny are provided in their respective headers
// (when_all.hpp, when_any.hpp). This file provides the simpler
// `Then` and `Map` combinators for basic composition.
//
// ============================================================================

#pragma once

#include <coroutine>
#include <type_traits>

#include "hotcoco/core/task.hpp"

namespace hotcoco {

// ============================================================================
// Then - Promise Chaining
// ============================================================================
//
// Attach a continuation function to a task. When the task completes,
// the function is called with the result and returns a new value.
//
// This creates a new Task that:
// 1. Awaits the input task
// 2. Applies the function to the result
// 3. Returns the transformed value
//

// Non-void input task
template <typename T, typename F>
    requires std::invocable<F, T>
Task<std::invoke_result_t<F, T>> Then(Task<T> task, F func) {
    using ResultT = std::invoke_result_t<F, T>;
    
    // Await the input task
    T value = co_await std::move(task);
    
    // Apply the transformation
    if constexpr (std::is_void_v<ResultT>) {
        func(std::move(value));
        co_return;
    } else {
        co_return func(std::move(value));
    }
}

// Void input task - function takes no arguments
template <typename F>
    requires std::invocable<F>
Task<std::invoke_result_t<F>> Then(Task<void> task, F func) {
    using ResultT = std::invoke_result_t<F>;
    
    // Await the input task
    co_await std::move(task);
    
    // Apply the transformation
    if constexpr (std::is_void_v<ResultT>) {
        func();
        co_return;
    } else {
        co_return func();
    }
}

// ============================================================================
// Map - Alias for Then with value transformation
// ============================================================================
// Just a semantic alias - Map emphasizes value transformation
//
template <typename T, typename F>
    requires std::invocable<F, T>
auto Map(Task<T> task, F func) {
    return Then(std::move(task), std::move(func));
}

}  // namespace hotcoco
