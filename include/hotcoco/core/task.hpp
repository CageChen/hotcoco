// ============================================================================
// hotcoco/core/task.hpp - Lazy Async Coroutine
// ============================================================================
//
// Task<T> is the fundamental coroutine type in hotcoco. It represents an
// asynchronous operation that will eventually produce a value of type T.
//
// KEY CONCEPTS:
// -------------
// 1. LAZY EXECUTION: A Task doesn't start executing until someone awaits it.
//    This is controlled by initial_suspend() returning suspend_always.
//
// 2. PROMISE TYPE: The compiler looks for a nested 'promise_type' to understand
//    how to create and manage the coroutine. We define customization points:
//    - get_return_object(): Creates the Task<T> that user code receives
//    - initial_suspend(): Should we start immediately or wait?
//    - final_suspend(): What happens when the coroutine finishes?
//    - return_value(): Handle co_return <value>
//    - unhandled_exception(): Required by standard; aborts under -fno-exceptions
//
// 3. COROUTINE HANDLE: std::coroutine_handle<Promise> is a pointer to the
//    coroutine's state (stack frame). We use it to resume/destroy the coroutine.
//
// 4. AWAITABLE: Task<T> is awaitable, meaning you can co_await it. The awaiter
//    (returned by operator co_await) defines what happens during suspension.
//
// USAGE:
// ------
//   Task<int> ComputeAsync() {
//       co_return 42;
//   }
//
//   Task<int> CallerAsync() {
//       int value = co_await ComputeAsync();  // Lazy: starts here
//       co_return value * 2;
//   }
//
// ============================================================================

#pragma once

#include "hotcoco/core/check.hpp"
#include "hotcoco/core/coroutine_compat.hpp"

#include <coroutine>
#include <cstdlib>
#include <optional>
#include <utility>

namespace hotcoco {

// Forward declaration - we define Task after Promise
template <typename T>
class Task;

// ============================================================================
// Promise Type
// ============================================================================
//
// The Promise is the "control block" of the coroutine. The compiler generates
// code that interacts with these methods at specific points in the coroutine's
// lifecycle.
//
// MEMORY MODEL:
// The promise lives inside the coroutine frame (compiler-allocated).
// It exists from coroutine creation until the frame is destroyed.
//
template <typename T>
class TaskPromise {
   public:
    // ========================================================================
    // CUSTOMIZATION POINT: get_return_object()
    // ========================================================================
    // Called immediately when the coroutine is created, BEFORE initial_suspend.
    // Returns the object that the caller receives (our Task<T>).
    //
    // We pass the coroutine_handle to Task so it can resume/destroy the coroutine.
    //
    Task<T> get_return_object() noexcept;

    // ========================================================================
    // CUSTOMIZATION POINT: initial_suspend()
    // ========================================================================
    // Called right after get_return_object(). The returned awaitable determines
    // whether the coroutine starts executing immediately or suspends.
    //
    // suspend_always -> LAZY: Coroutine suspends, waits for someone to resume it
    // suspend_never  -> EAGER: Coroutine runs immediately until first co_await
    //
    // WHY LAZY?
    // - Predictable: Execution starts when you co_await, not at creation
    // - Composable: You can create Tasks and pass them around before running
    // - Efficient: No work done until needed
    //
    std::suspend_always initial_suspend() noexcept { return {}; }

    // ========================================================================
    // CUSTOMIZATION POINT: final_suspend()
    // ========================================================================
    // Called when the coroutine completes (after co_return or exception).
    // The returned awaitable determines what happens next.
    //
    // CRITICAL: Must be noexcept! Throwing here is undefined behavior.
    //
    // We return a custom awaiter that:
    // 1. Always suspends (suspend_always behavior via await_ready() = false)
    // 2. Resumes the continuation (the coroutine that was waiting on us)
    //
    // This is called "symmetric transfer" - we directly transfer control to
    // the awaiting coroutine without going through the scheduler.
    //
    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }

        // SYMMETRIC TRANSFER:
        // Instead of returning void (which would return to the resumer),
        // we return a coroutine_handle to transfer control directly.
        // Returning noop_coroutine() means "just suspend, no transfer".
        //
        // NOTE: Under GCC+ASan, symmetric transfer is broken (GCC bug 100897).
        // SymmetricTransfer() falls back to explicit resume() in that case.
        //
        SymmetricTransferResult await_suspend(std::coroutine_handle<TaskPromise> finishing) noexcept {
            auto& promise = finishing.promise();
            if (promise.continuation_) {
                return SymmetricTransfer(promise.continuation_);
            }
            return SymmetricTransfer(std::noop_coroutine());
        }

        void await_resume() noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    // ========================================================================
    // CUSTOMIZATION POINT: return_value()
    // ========================================================================
    // Called when the coroutine executes: co_return <value>;
    // We store the value so the awaiter can retrieve it later.
    //
    void return_value(T value) noexcept { result_ = std::move(value); }

    // ========================================================================
    // CUSTOMIZATION POINT: unhandled_exception()
    // ========================================================================
    // Required by the standard but should never be reached under
    // -fno-exceptions. If somehow called, abort immediately.
    //
    void unhandled_exception() noexcept { std::abort(); }

    // ========================================================================
    // Result Access
    // ========================================================================
    // The awaiter calls this to get the result after the coroutine completes.
    //
    T& GetResult() & noexcept { return result_.value(); }
    T&& GetResult() && noexcept { return std::move(result_.value()); }

    // ========================================================================
    // Continuation Management
    // ========================================================================
    // When another coroutine co_awaits us, it becomes our "continuation".
    // We store it here so final_suspend can resume it when we complete.
    //
    void SetContinuation(std::coroutine_handle<> cont) noexcept {
        HOTCOCO_CHECK(!awaited_, "Task<T> co_awaited twice — this is undefined behavior");
        awaited_ = true;
        continuation_ = cont;
    }

   private:
    std::optional<T> result_;
    std::coroutine_handle<> continuation_;
    bool awaited_ = false;
};

// ============================================================================
// Task<T> - The Coroutine Return Type
// ============================================================================
//
// This is what users see and interact with. It's a thin wrapper around the
// coroutine_handle that provides:
// - RAII: Destroys the coroutine frame in destructor
// - Move-only: Coroutines are unique resources
// - Awaitable: Can be co_awaited
//
template <typename T>
class [[nodiscard("Task must be co_awaited")]] Task {
   public:
    // Required by the compiler to find our promise type
    using promise_type = TaskPromise<T>;
    using Handle = std::coroutine_handle<promise_type>;

    // ========================================================================
    // Construction / Destruction
    // ========================================================================

    explicit Task(Handle handle) noexcept : handle_(handle) {}

    ~Task() {
        if (handle_) {
            handle_.destroy();  // Clean up the coroutine frame
        }
    }

    // Move-only semantics (coroutines are unique resources)
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    // ========================================================================
    // Awaitable Interface
    // ========================================================================
    // operator co_await returns an awaiter object that defines how to
    // suspend/resume when someone does: co_await task;
    //
    struct Awaiter {
        Handle handle_;

        // AWAIT_READY:
        // Can we skip suspension? For lazy tasks, never - we always need to
        // run the coroutine first.
        bool await_ready() noexcept { return false; }

        // AWAIT_SUSPEND:
        // Called when we suspend. We:
        // 1. Store the awaiting coroutine as our continuation
        // 2. Resume our task (start its execution)
        //
        // The parameter 'awaiting' is the coroutine that is co_awaiting us.
        // We return our handle to transfer control to our task.
        //
        SymmetricTransferResult await_suspend(std::coroutine_handle<> awaiting) noexcept {
            handle_.promise().SetContinuation(awaiting);
            return SymmetricTransfer(handle_);
        }

        // AWAIT_RESUME:
        // Called when we resume (after our task completes and transfers back).
        // Return the task's result to the awaiting coroutine.
        //
        T await_resume() noexcept { return std::move(handle_.promise()).GetResult(); }
    };

    [[nodiscard]] Awaiter operator co_await() noexcept { return Awaiter{handle_}; }

    // ========================================================================
    // Direct Access (for SyncWait and testing)
    // ========================================================================
    [[nodiscard]] Handle GetHandle() const noexcept { return handle_; }

   private:
    Handle handle_;
};

// ============================================================================
// Promise::get_return_object() Implementation
// ============================================================================
// Defined here because it needs the complete Task type.
//
template <typename T>
Task<T> TaskPromise<T>::get_return_object() noexcept {
    return Task<T>{Task<T>::Handle::from_promise(*this)};
}

// ============================================================================
// Task<void> Specialization
// ============================================================================
// Void tasks don't return a value, just signal completion.
// We need a separate promise with return_void() instead of return_value().
//

template <>
class TaskPromise<void> {
   public:
    Task<void> get_return_object() noexcept;

    std::suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }
        SymmetricTransferResult await_suspend(std::coroutine_handle<TaskPromise<void>> finishing) noexcept {
            auto& promise = finishing.promise();
            if (promise.continuation_) {
                return SymmetricTransfer(promise.continuation_);
            }
            return SymmetricTransfer(std::noop_coroutine());
        }
        void await_resume() noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    // return_void() for co_return; (with no value)
    void return_void() noexcept {}

    void unhandled_exception() noexcept { std::abort(); }

    void GetResult() noexcept {}

    void SetContinuation(std::coroutine_handle<> cont) noexcept {
        HOTCOCO_CHECK(!awaited_, "Task<void> co_awaited twice — this is undefined behavior");
        awaited_ = true;
        continuation_ = cont;
    }

   private:
    std::coroutine_handle<> continuation_;
    bool awaited_ = false;
};

template <>
class [[nodiscard("Task must be co_awaited")]] Task<void> {
   public:
    using promise_type = TaskPromise<void>;
    using Handle = std::coroutine_handle<promise_type>;

    explicit Task(Handle handle) noexcept : handle_(handle) {}

    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    struct Awaiter {
        Handle handle_;

        bool await_ready() noexcept { return false; }

        SymmetricTransferResult await_suspend(std::coroutine_handle<> awaiting) noexcept {
            handle_.promise().SetContinuation(awaiting);
            return SymmetricTransfer(handle_);
        }

        void await_resume() noexcept { handle_.promise().GetResult(); }
    };

    [[nodiscard]] Awaiter operator co_await() noexcept { return Awaiter{handle_}; }

    [[nodiscard]] Handle GetHandle() const noexcept { return handle_; }

   private:
    Handle handle_;
};

inline Task<void> TaskPromise<void>::get_return_object() noexcept {
    return Task<void>{Task<void>::Handle::from_promise(*this)};
}

}  // namespace hotcoco
