// ============================================================================
// hotcoco/core/schedule_on.hpp - Executor Transfer and Yield
// ============================================================================
//
// Provides awaitables for transferring coroutines between executors and
// yielding control to allow other work to proceed.
//
// Yield():
//   Suspends the current coroutine and re-schedules it at the back of the
//   executor's queue. This gives other pending coroutines a chance to run,
//   preventing starvation in long-running computations.
//
// ScheduleOn(executor):
//   Suspends the current coroutine and resumes it on a different executor.
//   This enables cross-executor transfer (e.g., offloading CPU work to a
//   thread pool from an I/O executor).
//
// USAGE:
// ------
//   // Yield to let other coroutines run
//   co_await Yield();
//
//   // Transfer to a different executor
//   co_await ScheduleOn(thread_pool);
//
// ============================================================================

#pragma once

#include "hotcoco/io/executor.hpp"

#include <coroutine>

namespace hotcoco {

// ============================================================================
// YieldAwaitable - Cooperative yield point
// ============================================================================
//
// Suspends the coroutine and re-enqueues it on the current executor.
// Requires a current executor to be set (via ExecutorGuard or Run()).
//
class YieldAwaitable {
   public:
    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) const {
        Executor* executor = GetCurrentExecutor();
        if (!executor) {
            // No executor — don't suspend, resume immediately (degenerate case)
            return false;
        }
        executor->Schedule(handle);
        return true;
    }

    void await_resume() const noexcept {}
};

inline YieldAwaitable Yield() {
    return {};
}

// ============================================================================
// ScheduleOnAwaitable - Cross-executor transfer
// ============================================================================
//
// Suspends the coroutine and resumes it on the target executor.
// After resumption, GetCurrentExecutor() will return the target executor
// (assuming the target executor sets itself via ExecutorGuard in Run()).
//
class ScheduleOnAwaitable {
   public:
    explicit ScheduleOnAwaitable(Executor& target) : target_(target) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) const { target_.Schedule(handle); }

    void await_resume() const noexcept {}

   private:
    Executor& target_;
};

inline ScheduleOnAwaitable ScheduleOn(Executor& target) {
    return ScheduleOnAwaitable{target};
}

}  // namespace hotcoco
