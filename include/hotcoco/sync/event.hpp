// ============================================================================
// hotcoco/sync/event.hpp - Resettable Async Event
// ============================================================================
//
// AsyncEvent is a thread-safe, coroutine-aware manual-reset event.
// Multiple coroutines can co_await it. When Set() is called, all current
// waiters are resumed and future awaiters pass through immediately.
// Reset() returns it to the unset state for reuse.
//
// This is a foundational building block used by WhenAll (atomic countdown
// latch), WhenAny (first-completion notification), and TaskGroup.
//
// USAGE:
// ------
//   AsyncEvent event;
//
//   // Waiter coroutine
//   co_await event.Wait();  // Suspends until Set() is called
//
//   // Signaler
//   event.Set();            // Resumes all waiters
//
//   // Reuse
//   event.Reset();          // Back to unset state
//   co_await event.Wait();  // Suspends again
//
// ============================================================================

#pragma once

#include <coroutine>
#include <mutex>
#include <vector>

namespace hotcoco {

class AsyncEvent {
public:
    AsyncEvent() = default;

    // Non-copyable, non-movable
    AsyncEvent(const AsyncEvent&) = delete;
    AsyncEvent& operator=(const AsyncEvent&) = delete;
    AsyncEvent(AsyncEvent&&) = delete;
    AsyncEvent& operator=(AsyncEvent&&) = delete;

    // ========================================================================
    // WaitAwaitable
    // ========================================================================
    class WaitAwaitable {
    public:
        explicit WaitAwaitable(AsyncEvent& event) : event_(event) {}

        bool await_ready() noexcept {
            std::lock_guard<std::mutex> lock(event_.mutex_);
            return event_.signaled_;
        }

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            std::lock_guard<std::mutex> lock(event_.mutex_);
            if (event_.signaled_) {
                return false;  // Already set, don't suspend
            }
            event_.waiters_.push_back(h);
            return true;
        }

        void await_resume() noexcept {}

    private:
        AsyncEvent& event_;
    };

    // ========================================================================
    // Public API
    // ========================================================================

    WaitAwaitable Wait() {
        return WaitAwaitable(*this);
    }

    // Set the event, resuming all current waiters.
    // Future co_await Wait() calls will pass through immediately.
    void Set() {
        std::vector<std::coroutine_handle<>> to_wake;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (signaled_) {
                return;  // Already set
            }
            signaled_ = true;
            to_wake = std::move(waiters_);
        }
        for (auto h : to_wake) {
            h.resume();
        }
    }

    // Reset the event to unset state.
    // New co_await Wait() calls will suspend until Set() is called again.
    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        signaled_ = false;
    }

    // Check if the event is currently set.
    bool IsSet() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return signaled_;
    }

private:
    mutable std::mutex mutex_;
    bool signaled_ = false;
    std::vector<std::coroutine_handle<>> waiters_;
};

}  // namespace hotcoco
