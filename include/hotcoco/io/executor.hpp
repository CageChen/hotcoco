// ============================================================================
// hotcoco/io/executor.hpp - Abstract Event Loop Interface
// ============================================================================
//
// The Executor is the heart of async I/O in hotcoco. It provides an abstraction
// over event loops (like libuv, io_uring, epoll) that can:
// - Run pending tasks
// - Schedule coroutines for later execution
// - Handle timers for AsyncSleep
//
// DESIGN PHILOSOPHY:
// ------------------
// 1. SINGLE-THREADED: Each Executor runs on one thread. This simplifies
//    coroutine safety - no locks needed within a single executor.
//
// 2. PLUGGABLE BACKENDS: The abstract interface allows different implementations
//    (LibuvExecutor, IoUringExecutor, ThreadPoolExecutor) without changing user code.
//
// 3. COROUTINE-FIRST: The API is designed around coroutine_handle scheduling,
//    making it natural to integrate with awaitables.
//
// USAGE:
// ------
//   // Get the current thread's executor
//   Executor& exec = GetCurrentExecutor();
//
//   // Schedule a coroutine to run
//   exec.Schedule(some_coroutine_handle);
//
//   // Schedule with delay
//   exec.ScheduleAfter(100ms, some_coroutine_handle);
//
//   // Run the event loop
//   exec.Run();
//
// ============================================================================

#pragma once

#include <chrono>
#include <coroutine>
#include <functional>
#include <memory>

namespace hotcoco {

// ============================================================================
// Executor - Abstract Event Loop Interface
// ============================================================================
class Executor {
   public:
    virtual ~Executor() = default;

    // ========================================================================
    // Event Loop Control
    // ========================================================================

    // Run the event loop until Stop() is called or no more work
    virtual void Run() = 0;

    // Run one iteration of the event loop (non-blocking)
    virtual void RunOnce() = 0;

    // Signal the event loop to stop
    virtual void Stop() = 0;

    // Check if the executor is running
    [[nodiscard]] virtual bool IsRunning() const = 0;

    // ========================================================================
    // Coroutine Scheduling
    // ========================================================================

    // Schedule a coroutine to run as soon as possible
    // This is the primary way to resume suspended coroutines
    virtual void Schedule(std::coroutine_handle<> handle) = 0;

    // Schedule a coroutine to run after a delay
    // Used by AsyncSleep and timer-based operations
    virtual void ScheduleAfter(std::chrono::milliseconds delay, std::coroutine_handle<> handle) = 0;

    // ========================================================================
    // Callbacks (for non-coroutine integration)
    // ========================================================================

    // Post a callback to run on the event loop
    virtual void Post(std::function<void()> callback) = 0;
};

// ============================================================================
// Thread-Local Executor Access
// ============================================================================
// Each thread can have one "current" executor. This allows awaitables to
// find the executor without explicit passing.
//

// Get the current thread's executor (nullptr if none set)
[[nodiscard]] Executor* GetCurrentExecutor();

// Set the current thread's executor (used internally by Run())
void SetCurrentExecutor(Executor* executor);

// RAII guard for setting/restoring current executor
class ExecutorGuard {
   public:
    explicit ExecutorGuard(Executor* executor);
    ~ExecutorGuard();

    ExecutorGuard(const ExecutorGuard&) = delete;
    ExecutorGuard& operator=(const ExecutorGuard&) = delete;

   private:
    Executor* previous_;
};

}  // namespace hotcoco
