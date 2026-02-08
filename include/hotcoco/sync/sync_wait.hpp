// ============================================================================
// hotcoco/sync/sync_wait.hpp - Blocking Wait for Coroutines
// ============================================================================
//
// SyncWait() blocks the current thread until a coroutine completes, then
// returns its result. This is the bridge between synchronous code (like main())
// and async coroutines.
//
// HOW IT WORKS:
// -------------
// 1. Create a simple event (binary semaphore) for signaling completion
// 2. Wrap the task in a "runner" coroutine that signals when done
// 3. Start the runner and block on the event
// 4. When the task completes, the runner signals and we wake up
//
// USAGE:
// ------
//   Task<int> ComputeAsync() { co_return 42; }
//
//   int main() {
//       int result = SyncWait(ComputeAsync());
//       std::cout << result << std::endl;  // 42
//   }
//
// WARNING:
// --------
// Never call SyncWait from inside a coroutine! It will block the thread and
// potentially cause deadlocks. SyncWait is only for the sync/async boundary.
//
// ============================================================================

#pragma once

#include "hotcoco/core/task.hpp"

#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>

namespace hotcoco {

namespace detail {

// ============================================================================
// SyncWaitEvent - Simple Binary Semaphore
// ============================================================================
// A minimal synchronization primitive for one-shot signaling.
//
class SyncWaitEvent {
   public:
    void Signal() {
        std::lock_guard lock(mutex_);
        signaled_ = true;
        cv_.notify_one();
    }

    void Wait() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return signaled_; });
    }

   private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool signaled_ = false;
};

// ============================================================================
// SyncWaitTask - Runner Coroutine
// ============================================================================
// This coroutine runs the user's task and signals completion.
// It's internal and not part of the public API.
//

template <typename T>
class SyncWaitTask;

template <typename T>
class SyncWaitPromise {
   public:
    SyncWaitTask<T> get_return_object() noexcept;

    // Start immediately (eager) - we want to run right away
    std::suspend_never initial_suspend() noexcept { return {}; }

    // Don't suspend at the end - just finish
    std::suspend_never final_suspend() noexcept { return {}; }

    void return_void() noexcept {}

    void unhandled_exception() noexcept { std::abort(); }

    void SetEvent(SyncWaitEvent* event) noexcept { event_ = event; }
    void SetResult(T* result) noexcept { result_ = result; }

    void StoreResult(T value) {
        if (result_) {
            *result_ = std::move(value);
        }
    }

    void SignalCompletion() {
        if (event_) {
            event_->Signal();
        }
    }

   private:
    SyncWaitEvent* event_ = nullptr;
    T* result_ = nullptr;
};

template <typename T>
class SyncWaitTask {
   public:
    using promise_type = SyncWaitPromise<T>;
    using Handle = std::coroutine_handle<promise_type>;

    explicit SyncWaitTask(Handle handle) noexcept : handle_(handle) {}

    // No destructor needed - final_suspend is suspend_never so coroutine
    // destroys itself

    SyncWaitTask(const SyncWaitTask&) = delete;
    SyncWaitTask& operator=(const SyncWaitTask&) = delete;
    SyncWaitTask(SyncWaitTask&&) = default;
    SyncWaitTask& operator=(SyncWaitTask&&) = default;

    promise_type& GetPromise() { return handle_.promise(); }

   private:
    Handle handle_;
};

template <typename T>
SyncWaitTask<T> SyncWaitPromise<T>::get_return_object() noexcept {
    return SyncWaitTask<T>{SyncWaitTask<T>::Handle::from_promise(*this)};
}

}  // namespace detail

// ============================================================================
// SyncWait - Public API
// ============================================================================
//
// Blocks until the task completes and returns its result.
//
// WARNING:
// --------
// SyncWait resumes the task inline on the calling thread. If the task
// internally co_awaits an operation that requires an executor (e.g.,
// AsyncSleep, TcpStream::Read), it will deadlock because no executor
// is running to process the event. Only use SyncWait with tasks that
// complete synchronously or that run their own executor internally.
//
template <typename T>
T SyncWait(Task<T> task) {
    detail::SyncWaitEvent event;
    std::optional<T> result;

    // Create a simple wrapper that runs the task
    auto wrapper = [&]() -> Task<void> {
        result = co_await std::move(task);
        event.Signal();
    };

    // Start the wrapper
    auto wrapper_task = wrapper();
    wrapper_task.GetHandle().resume();

    // Wait for completion
    event.Wait();

    return std::move(*result);
}

// Specialization for void tasks
inline void SyncWait(Task<void> task) {
    detail::SyncWaitEvent event;

    auto wrapper = [&]() -> Task<void> {
        co_await std::move(task);
        event.Signal();
    };

    auto wrapper_task = wrapper();
    wrapper_task.GetHandle().resume();

    event.Wait();
}

}  // namespace hotcoco
