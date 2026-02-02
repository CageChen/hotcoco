// ============================================================================
// hotcoco/io/timer.hpp - Async Timer Utilities
// ============================================================================
//
// This header provides AsyncSleep, the async equivalent of std::this_thread::sleep_for.
// Instead of blocking the thread, AsyncSleep suspends the coroutine and resumes
// it after the specified duration.
//
// HOW IT WORKS:
// -------------
// 1. AsyncSleep creates an awaitable object
// 2. When co_awaited, it:
//    a. Gets the current executor
//    b. Schedules resumption after the delay
//    c. Suspends the coroutine
// 3. When the timer fires, the executor resumes the coroutine
//
// USAGE:
// ------
//   Task<void> MyTask() {
//       std::cout << "Starting..." << std::endl;
//       co_await AsyncSleep(1000ms);  // Sleep for 1 second
//       std::cout << "Done!" << std::endl;
//   }
//
// ============================================================================

#pragma once

#include <chrono>
#include <coroutine>

#include "hotcoco/io/executor.hpp"

namespace hotcoco {

// ============================================================================
// AsyncSleep Awaitable
// ============================================================================
//
// This is an awaitable that suspends the coroutine for a specified duration.
// When resumed, it returns void (no value).
//
class AsyncSleep {
public:
    // ========================================================================
    // Construction
    // ========================================================================

    explicit AsyncSleep(std::chrono::milliseconds duration)
        : duration_(duration) {}

    // Convenience constructors for different duration types
    template <typename Rep, typename Period>
    explicit AsyncSleep(std::chrono::duration<Rep, Period> duration)
        : duration_(std::chrono::duration_cast<std::chrono::milliseconds>(duration)) {}

    // ========================================================================
    // Awaitable Interface
    // ========================================================================

    // Never ready immediately - always need to wait
    bool await_ready() const noexcept { return false; }

    // Schedule resumption after the delay
    bool await_suspend(std::coroutine_handle<> handle) const {
        // Get the current executor (must be set by the event loop)
        Executor* executor = GetCurrentExecutor();

        // If no executor, we can't sleep - don't suspend, resume immediately
        // In practice, this shouldn't happen if used correctly
        if (!executor) {
            return false;
        }

        // Schedule the coroutine to resume after the delay
        executor->ScheduleAfter(duration_, handle);
        return true;
    }

    // Nothing to return after sleeping
    void await_resume() const noexcept {}

private:
    std::chrono::milliseconds duration_;
};

// ============================================================================
// Convenience Literals
// ============================================================================
// These allow writing: co_await 100ms instead of co_await AsyncSleep(100ms)
// (Requires using namespace hotcoco::literals)
//

namespace literals {

inline AsyncSleep operator""_sleep(unsigned long long ms) {
    return AsyncSleep(std::chrono::milliseconds(ms));
}

}  // namespace literals

}  // namespace hotcoco
