// ============================================================================
// hotcoco/io/libuv_executor.hpp - libuv-based Event Loop
// ============================================================================
//
// LibuvExecutor is the first concrete implementation of Executor, using libuv
// as the underlying event loop. libuv is battle-tested (used by Node.js) and
// provides cross-platform async I/O.
//
// WHY LIBUV:
// ----------
// 1. Mature and stable - powers Node.js
// 2. Cross-platform (though we only target Linux)
// 3. Simple C API that's easy to wrap
// 4. Built-in timer support for AsyncSleep
//
// ARCHITECTURE:
// -------------
// - uv_loop_t: The libuv event loop
// - uv_async_t: For thread-safe scheduling from other threads
// - uv_timer_t: For delayed scheduling (AsyncSleep)
// - uv_idle_t: For immediate coroutine resumption
//
// ============================================================================

#pragma once

#include "hotcoco/core/error.hpp"
#include "hotcoco/core/result.hpp"
#include "hotcoco/io/executor.hpp"

#include <atomic>
#include <chrono>
#include <coroutine>
#include <memory>
#include <mutex>
#include <queue>
#include <uv.h>
#include <vector>

namespace hotcoco {

// ============================================================================
// LibuvExecutor - libuv-based Executor Implementation
// ============================================================================
class LibuvExecutor : public Executor {
   public:
    // Factory method — returns an error if libuv initialization fails.
    static Result<std::unique_ptr<LibuvExecutor>, std::error_code> Create();

    ~LibuvExecutor() override;

    // Non-copyable, non-movable (libuv handles can't be moved)
    LibuvExecutor(const LibuvExecutor&) = delete;
    LibuvExecutor& operator=(const LibuvExecutor&) = delete;

    // ========================================================================
    // Executor Interface Implementation
    // ========================================================================

    void Run() override;
    void RunOnce() override;
    void Stop() override;
    bool IsRunning() const override;

    void Schedule(std::coroutine_handle<> handle) override;
    void ScheduleAfter(std::chrono::milliseconds delay, std::coroutine_handle<> handle) override;
    void Post(std::function<void()> callback) override;

    // Access the underlying libuv loop (for TCP, etc.)
    uv_loop_t* GetLoop() { return &loop_; }

   private:
    // Private constructor — use Create() factory method
    LibuvExecutor();

    // ========================================================================
    // Internal Types
    // ========================================================================

    // Represents a scheduled coroutine with optional delay
    struct ScheduledHandle {
        std::coroutine_handle<> handle;
        uv_timer_t* timer = nullptr;  // Non-null if delayed
    };

    // ========================================================================
    // libuv Callbacks
    // ========================================================================
    static void OnAsync(uv_async_t* handle);
    static void OnTimer(uv_timer_t* handle);
    static void OnIdle(uv_idle_t* handle);
    static void OnClose(uv_handle_t* handle);

    // Process all ready coroutines
    void ProcessReadyQueue();

    // ========================================================================
    // State
    // ========================================================================
    uv_loop_t loop_;
    uv_async_t async_;  // For cross-thread scheduling
    uv_idle_t idle_;    // For processing ready queue
    std::atomic<bool> running_{false};
    bool idle_active_ = false;

    // Queue of coroutines ready to run (protected by mutex for thread-safety)
    std::mutex queue_mutex_;
    std::queue<std::coroutine_handle<>> ready_queue_;

    // Queue of callbacks to run
    std::queue<std::function<void()>> callback_queue_;

    // Queue of pending timer requests (for thread-safe ScheduleAfter)
    struct TimerRequest {
        std::chrono::milliseconds delay;
        std::coroutine_handle<> handle;
    };
    std::queue<TimerRequest> timer_queue_;
};

}  // namespace hotcoco
