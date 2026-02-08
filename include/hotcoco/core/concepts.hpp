// ============================================================================
// hotcoco/core/concepts.hpp - C++20 Awaitable/Awaiter Concepts
// ============================================================================
//
// Formal concept definitions for the coroutine awaitable and awaiter protocols.
// These enable better compiler error messages and allow generic combinators
// to accept any awaitable (not just Task<T>).
//
// CONCEPTS:
// ---------
//   Awaiter<T>       - Type with await_ready/await_suspend/await_resume
//   Awaitable<T>     - Type that is an Awaiter, or has operator co_await()
//   AwaitableOf<T,R> - Awaitable whose await_resume() returns R
//
// USAGE:
// ------
//   template <Awaitable A>
//   void Process(A&& awaitable);
//
//   template <AwaitableOf<int> A>
//   Task<int> Transform(A&& awaitable);
//
// ============================================================================

#pragma once

#include <coroutine>
#include <type_traits>
#include <utility>

namespace hotcoco {

// ============================================================================
// Awaiter concept
// ============================================================================
//
// An Awaiter is a type that directly implements the three awaiter methods:
//   - await_ready()   -> bool
//   - await_suspend() -> void, bool, or coroutine_handle<>
//   - await_resume()  -> any type
//
// The await_suspend parameter can be any coroutine_handle specialization,
// but we check against coroutine_handle<> (type-erased) which all handles
// are convertible to.
//

template <typename T>
concept Awaiter = requires(T t, std::coroutine_handle<> h) {
    { t.await_ready() } -> std::convertible_to<bool>;
    { t.await_resume() };
} && (
    requires(T t, std::coroutine_handle<> h) {
        { t.await_suspend(h) } -> std::same_as<void>;
    } ||
    requires(T t, std::coroutine_handle<> h) {
        { t.await_suspend(h) } -> std::same_as<bool>;
    } ||
    requires(T t, std::coroutine_handle<> h) {
        { t.await_suspend(h) } -> std::convertible_to<std::coroutine_handle<>>;
    }
);

// ============================================================================
// Helper: get the awaiter from a type
// ============================================================================
//
// The C++ coroutine machinery resolves an awaitable to an awaiter via:
//   1. Member operator co_await()
//   2. Free-function operator co_await()
//   3. The object itself (if it's already an awaiter)
//

namespace detail {

template <typename T>
concept HasMemberCoAwait = requires(T t) {
    { t.operator co_await() };
};

template <typename T>
concept HasFreeCoAwait = requires(T t) {
    { operator co_await(t) };
};

// Get the awaiter type from an awaitable
template <typename T>
decltype(auto) GetAwaiter(T&& t) {
    if constexpr (HasMemberCoAwait<T>) {
        return std::forward<T>(t).operator co_await();
    } else if constexpr (HasFreeCoAwait<T>) {
        return operator co_await(std::forward<T>(t));
    } else {
        return std::forward<T>(t);
    }
}

template <typename T>
using AwaiterType = decltype(GetAwaiter(std::declval<T>()));

}  // namespace detail

// ============================================================================
// Awaitable concept
// ============================================================================
//
// An Awaitable is a type that can be used with co_await. It is either:
//   1. Already an Awaiter
//   2. Has a member operator co_await() returning an Awaiter
//   3. Has a free-function operator co_await() returning an Awaiter
//

template <typename T>
concept Awaitable = Awaiter<T> || (detail::HasMemberCoAwait<T> && Awaiter<detail::AwaiterType<T>>) ||
                    (detail::HasFreeCoAwait<T> && Awaiter<detail::AwaiterType<T>>);

// ============================================================================
// AwaitResult - trait to get the return type of co_await
// ============================================================================

template <Awaitable T>
using AwaitResult = decltype(std::declval<detail::AwaiterType<T>>().await_resume());

// ============================================================================
// AwaitableOf concept
// ============================================================================
//
// An Awaitable whose await_resume() returns a type convertible to R.
//

template <typename T, typename R>
concept AwaitableOf = Awaitable<T> && requires { requires std::convertible_to<AwaitResult<T>, R>; };

// Specialization: AwaitableOf<T, void> matches any awaitable returning void
template <typename T>
concept AwaitableOfVoid = Awaitable<T> && std::is_void_v<AwaitResult<T>>;

}  // namespace hotcoco
