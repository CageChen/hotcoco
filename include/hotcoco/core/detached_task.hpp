// ============================================================================
// hotcoco/core/detached_task.hpp - Self-Destroying Coroutine
// ============================================================================
//
// DetachedTask is a coroutine type whose frame is automatically destroyed
// by the C++ runtime when it completes. No external owner needs to call
// .destroy(). This is achieved by having final_suspend() return
// suspend_never after invoking an optional completion callback.
//
// This is a building block for WhenAny (controller task that outlives the
// caller) and TaskGroup (fire-and-forget tasks with completion tracking).
//
// KEY DESIGN:
// -----------
// - initial_suspend -> suspend_always (lazy start)
// - final_suspend -> invoke callback, then suspend_never (self-destruct)
// - Destructor is a no-op (the runtime handles cleanup)
// - SetCallback() registers a function called just before self-destruction
//
// USAGE:
// ------
//   auto detached = MakeDetached(SomeTask());
//   detached.SetCallback([]() { std::cout << "Done!" << std::endl; });
//   detached.GetHandle().resume();  // Start it; it will self-destruct
//
// ============================================================================

#pragma once

#include "hotcoco/core/task.hpp"

#include <coroutine>
#include <cstdlib>
#include <functional>

namespace hotcoco {

class DetachedTask {
   public:
    struct promise_type {
        std::function<void()> callback;

        DetachedTask get_return_object() noexcept {
            return DetachedTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        auto final_suspend() noexcept {
            struct FinalAwaiter {
                bool await_ready() noexcept { return false; }
                void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    auto& promise = h.promise();
                    if (promise.callback) {
                        promise.callback();
                    }
                    // Return to the runtime which will destroy the frame
                    // because we use suspend_never-like behavior below.
                    // Actually we use await_suspend returning void +
                    // await_ready=false pattern, but we need to destroy
                    // the handle explicitly since await_suspend(void)
                    // means "always suspend".
                    h.destroy();
                }
                void await_resume() noexcept {}
            };
            return FinalAwaiter{};
        }

        void return_void() noexcept {}

        void unhandled_exception() noexcept { std::abort(); }
    };

    using Handle = std::coroutine_handle<promise_type>;

    explicit DetachedTask(Handle h) noexcept : handle_(h) {}

    // Move-only
    DetachedTask(DetachedTask&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    DetachedTask& operator=(DetachedTask&& other) noexcept {
        if (this != &other) {
            if (handle_ && !started_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, nullptr);
            started_ = other.started_;
        }
        return *this;
    }

    DetachedTask(const DetachedTask&) = delete;
    DetachedTask& operator=(const DetachedTask&) = delete;

    // Destructor is a no-op: final_suspend handles cleanup for started
    // coroutines. For never-started coroutines, we must destroy manually.
    ~DetachedTask() {
        if (handle_ && !started_) {
            handle_.destroy();
        }
    }

    void SetCallback(std::function<void()> cb) {
        if (handle_) {
            handle_.promise().callback = std::move(cb);
        }
    }

    Handle GetHandle() const noexcept { return handle_; }

    void Start() {
        if (handle_) {
            started_ = true;
            handle_.resume();
        }
    }

   private:
    Handle handle_;
    bool started_ = false;
};

// Helper: wrap a Task<void> into a DetachedTask
inline DetachedTask MakeDetached(Task<void> task) {
    co_await std::move(task);
}

}  // namespace hotcoco
