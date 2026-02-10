// ============================================================================
// hotcoco/execution/as_awaitable.hpp - P2300 Sender → co_await Bridge
// ============================================================================
//
// Lets any P2300 sender be co_awaited inside a hotcoco coroutine.
// The adapter connects the sender with an internal receiver that stores
// the result and resumes the awaiting coroutine on completion.
//
// USAGE:
// ------
//   Task<int> MyCoroutine() {
//       auto sender = stdexec::just(42)
//                   | stdexec::then([](int x) { return x * 2; });
//
//       int result = co_await hotcoco::execution::AsAwaitable(std::move(sender));
//       co_return result;  // 84
//   }
//
// ============================================================================

#pragma once

#include "hotcoco/core/check.hpp"

#include <coroutine>
#include <cstdlib>
#include <new>
#include <optional>
#include <stdexec/execution.hpp>
#include <type_traits>
#include <utility>

namespace hotcoco::execution {

namespace detail {

// ============================================================================
// Value type deduction from sender completion signatures
// ============================================================================
template <typename Sender>
struct SenderValueType {
    // Default: void sender
    using type = void;
};

// Specialization: extract T from completion_signatures<set_value_t(T)>
template <typename Sender>
    requires requires {
        typename stdexec::value_types_of_t<Sender, stdexec::env<>, std::type_identity_t, std::type_identity_t>;
    }
struct SenderValueType<Sender> {
    using type = stdexec::value_types_of_t<Sender, stdexec::env<>, std::type_identity_t, std::type_identity_t>;
};

template <typename Sender>
using sender_value_t = typename SenderValueType<Sender>::type;

// ============================================================================
// BridgeReceiver - receives completion from the sender
// ============================================================================
template <typename T>
struct AwaitableState {
    std::optional<T> result;
    std::coroutine_handle<> continuation;
    bool stopped = false;
};

template <>
struct AwaitableState<void> {
    std::coroutine_handle<> continuation;
    bool completed = false;
    bool stopped = false;
};

template <typename T>
struct BridgeReceiver {
    using receiver_concept = stdexec::receiver_t;

    AwaitableState<T>* state;

    void set_value(T val) noexcept {
        state->result.emplace(std::move(val));
        state->continuation.resume();
    }

    void set_stopped() noexcept {
        state->stopped = true;
        state->continuation.resume();
    }

    void set_error(auto&&) noexcept {
        // Under -fno-exceptions, errors that reach here are unrecoverable
        std::abort();
    }

    auto get_env() const noexcept { return stdexec::env<>{}; }
};

template <>
struct BridgeReceiver<void> {
    using receiver_concept = stdexec::receiver_t;

    AwaitableState<void>* state;

    void set_value() noexcept {
        state->completed = true;
        state->continuation.resume();
    }

    void set_stopped() noexcept {
        state->stopped = true;
        state->continuation.resume();
    }

    void set_error(auto&&) noexcept { std::abort(); }

    auto get_env() const noexcept { return stdexec::env<>{}; }
};

}  // namespace detail

// ============================================================================
// SenderAwaitable<Sender> - makes any sender co_awaitable
// ============================================================================
// Uses a union for deferred construction of the immovable operation state.
// The operation state is constructed in await_suspend via guaranteed copy
// elision from the stdexec::connect() prvalue.
template <typename Sender, typename T = detail::sender_value_t<std::remove_cvref_t<Sender>>>
class SenderAwaitable {
    using receiver_t = detail::BridgeReceiver<T>;
    using op_state_t = stdexec::connect_result_t<Sender, receiver_t>;

   public:
    explicit SenderAwaitable(Sender sender) noexcept : sender_(std::move(sender)), has_op_(false) {}

    ~SenderAwaitable() {
        if (has_op_) {
            op_.~op_state_t();
        }
    }

    // Non-movable: op_state_t is immovable, and moving after await_suspend
    // would leave the operation state pointing at stale memory.
    SenderAwaitable(SenderAwaitable&&) = delete;
    SenderAwaitable& operator=(SenderAwaitable&&) = delete;

    bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> cont) noexcept {
        state_.continuation = cont;
        // Raw placement-new with a prvalue argument enables guaranteed copy
        // elision (C++17 [dcl.init]/17.6.1). std::construct_at cannot be used
        // here because its SFINAE constraint checks move-constructibility,
        // which is deleted for stdexec operation states.
        ::new (static_cast<void*>(&op_)) op_state_t(stdexec::connect(std::move(sender_), receiver_t{&state_}));
        has_op_ = true;
        stdexec::start(op_);
    }

    T await_resume() noexcept
        requires(!std::is_void_v<T>)
    {
        HOTCOCO_CHECK(!state_.stopped, "sender was stopped — cannot produce a value");
        return std::move(*state_.result);
    }

    void await_resume() noexcept
        requires(std::is_void_v<T>)
    {
        HOTCOCO_CHECK(!state_.stopped, "sender was stopped — cannot produce a value");
    }

   private:
    Sender sender_;
    detail::AwaitableState<T> state_;
    union {
        op_state_t op_;
    };
    bool has_op_;
};

// Factory function
template <typename Sender>
auto AsAwaitable(Sender sender) noexcept {
    return SenderAwaitable<Sender>{std::move(sender)};
}

}  // namespace hotcoco::execution
