// ============================================================================
// hotcoco/core/spawn.hpp - Fire-and-Forget Coroutine Execution
// ============================================================================
//
// Spawn() allows running a coroutine without awaiting it - "fire and forget".
// The spawned coroutine's lifetime is managed independently, allowing true
// parallel execution with the spawning context.
//
// DESIGN PHILOSOPHY:
// ------------------
// 1. DETACHED LIFETIME: Coroutine runs independently of caller
// 2. COMPLETION NOTIFICATION: Optional callback when done
// 3. ERROR HANDLING: Exceptions captured and reported
//
// USAGE:
// ------
//   // Fire and forget
//   Spawn(executor, BackgroundWork());
//
//   // With completion callback
//   Spawn(executor, FetchData())
//       .OnComplete([](Result result) {
//           std::cout << "Got: " << result << std::endl;
//       });
//
// ============================================================================

#pragma once

#include <coroutine>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>

#include "hotcoco/core/error.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/io/executor.hpp"

namespace hotcoco {

// ============================================================================
// SpawnState - Shared state for spawned task
// ============================================================================
template <typename T>
struct SpawnState {
    std::mutex mutex;
    std::optional<T> result;
    std::error_code error;
    std::function<void(T)> on_complete;
    std::function<void(std::error_code)> on_error;
    bool completed = false;
    bool failed = false;

    void Complete(T value) {
        std::function<void(T)> cb;
        std::optional<T> val;
        {
            std::lock_guard<std::mutex> lock(mutex);
            result = std::move(value);
            completed = true;
            if (on_complete) {
                cb = std::move(on_complete);
                val = std::move(*result);
            }
        }
        if (cb) {
            cb(std::move(*val));
        }
    }

    void Fail(std::error_code ec) {
        std::function<void(std::error_code)> cb;
        {
            std::lock_guard<std::mutex> lock(mutex);
            error = ec;
            failed = true;
            if (on_error) {
                cb = std::move(on_error);
            }
        }
        if (cb) {
            cb(ec);
        }
    }
};

template <>
struct SpawnState<void> {
    std::mutex mutex;
    bool completed = false;
    bool failed = false;
    std::error_code error;
    std::function<void()> on_complete;
    std::function<void(std::error_code)> on_error;

    void Complete() {
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lock(mutex);
            completed = true;
            if (on_complete) {
                cb = std::move(on_complete);
            }
        }
        if (cb) {
            cb();
        }
    }

    void Fail(std::error_code ec) {
        std::function<void(std::error_code)> cb;
        {
            std::lock_guard<std::mutex> lock(mutex);
            error = ec;
            failed = true;
            if (on_error) {
                cb = std::move(on_error);
            }
        }
        if (cb) {
            cb(ec);
        }
    }
};

// ============================================================================
// SpawnHandle - Handle to a spawned task
// ============================================================================
template <typename T>
class SpawnHandle {
public:
    explicit SpawnHandle(std::shared_ptr<SpawnState<T>> state)
        : state_(std::move(state)) {}
    
    // Register completion callback
    SpawnHandle& OnComplete(std::function<void(T)> callback) {
        if (state_) {
            std::optional<T> value_copy;
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (state_->completed) {
                    // Already completed — copy under lock, invoke outside
                    value_copy = state_->result;
                } else {
                    // Not yet completed — store for later
                    state_->on_complete = std::move(callback);
                }
            }
            if (value_copy) {
                callback(std::move(*value_copy));
            }
        }
        return *this;
    }

    // Register error callback
    SpawnHandle& OnError(std::function<void(std::error_code)> callback) {
        if (state_) {
            std::error_code ec;
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (state_->failed) {
                    ec = state_->error;
                } else {
                    state_->on_error = std::move(callback);
                }
            }
            if (ec) {
                callback(ec);
            }
        }
        return *this;
    }

private:
    std::shared_ptr<SpawnState<T>> state_;
};

template <>
class SpawnHandle<void> {
public:
    explicit SpawnHandle(std::shared_ptr<SpawnState<void>> state)
        : state_(std::move(state)) {}
    
    SpawnHandle& OnComplete(std::function<void()> callback) {
        if (state_) {
            bool already_done = false;
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (state_->completed) {
                    already_done = true;
                } else {
                    state_->on_complete = std::move(callback);
                }
            }
            if (already_done) {
                callback();
            }
        }
        return *this;
    }

    SpawnHandle& OnError(std::function<void(std::error_code)> callback) {
        if (state_) {
            std::error_code ec;
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (state_->failed) {
                    ec = state_->error;
                } else {
                    state_->on_error = std::move(callback);
                }
            }
            if (ec) {
                callback(ec);
            }
        }
        return *this;
    }

private:
    std::shared_ptr<SpawnState<void>> state_;
};

// ============================================================================
// SpawnedTask - Coroutine wrapper for spawned execution
// ============================================================================
template <typename T>
class SpawnedTask {
public:
    struct promise_type {
        std::shared_ptr<SpawnState<T>> state;
        
        SpawnedTask get_return_object() {
            return SpawnedTask{
                std::coroutine_handle<promise_type>::from_promise(*this)
            };
        }
        
        std::suspend_always initial_suspend() noexcept { return {}; }
        
        auto final_suspend() noexcept {
            struct FinalAwaiter {
                bool await_ready() noexcept { return false; }
                void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    // Destroy the coroutine after completion
                    h.destroy();
                }
                void await_resume() noexcept {}
            };
            return FinalAwaiter{};
        }
        
        void unhandled_exception() noexcept { std::abort(); }

        template <typename U>
        void return_value(U&& value) {
            if (state) {
                state->Complete(std::forward<U>(value));
            }
        }
    };
    
    using handle_type = std::coroutine_handle<promise_type>;
    
    explicit SpawnedTask(handle_type h) : handle_(h) {}
    
    // Move only
    SpawnedTask(SpawnedTask&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    SpawnedTask& operator=(SpawnedTask&& other) noexcept {
        if (this != &other) {
            // Started coroutines self-destruct via final_suspend;
            // only destroy handles that were never started (still at
            // initial_suspend).  handle_.done() is true after
            // final_suspend, meaning the frame was already destroyed
            // by final_suspend's h.destroy().
            if (handle_ && !handle_.done()) {
                handle_.destroy();
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }
    SpawnedTask(const SpawnedTask&) = delete;
    SpawnedTask& operator=(const SpawnedTask&) = delete;

    ~SpawnedTask() {
        // Don't destroy - final_suspend handles it for started coroutines
        // Unstarted coroutines are cleaned up by move-assignment
    }

    void SetState(std::shared_ptr<SpawnState<T>> state) {
        if (handle_) {
            handle_.promise().state = std::move(state);
        }
    }
    
    handle_type GetHandle() const { return handle_; }
    
private:
    handle_type handle_;
};

template <>
class SpawnedTask<void> {
public:
    struct promise_type {
        std::shared_ptr<SpawnState<void>> state;
        
        SpawnedTask get_return_object() {
            return SpawnedTask{
                std::coroutine_handle<promise_type>::from_promise(*this)
            };
        }
        
        std::suspend_always initial_suspend() noexcept { return {}; }
        
        auto final_suspend() noexcept {
            struct FinalAwaiter {
                bool await_ready() noexcept { return false; }
                void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    h.destroy();
                }
                void await_resume() noexcept {}
            };
            return FinalAwaiter{};
        }
        
        void unhandled_exception() noexcept { std::abort(); }

        void return_void() {
            if (state) {
                state->Complete();
            }
        }
    };
    
    using handle_type = std::coroutine_handle<promise_type>;
    
    explicit SpawnedTask(handle_type h) : handle_(h) {}
    
    SpawnedTask(SpawnedTask&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    SpawnedTask& operator=(SpawnedTask&& other) noexcept {
        if (this != &other) {
            if (handle_ && !handle_.done()) {
                handle_.destroy();
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }
    SpawnedTask(const SpawnedTask&) = delete;
    SpawnedTask& operator=(const SpawnedTask&) = delete;

    ~SpawnedTask() {}

    void SetState(std::shared_ptr<SpawnState<void>> state) {
        if (handle_) {
            handle_.promise().state = std::move(state);
        }
    }
    
    handle_type GetHandle() const { return handle_; }
    
private:
    handle_type handle_;
};

// ============================================================================
// Spawn Functions
// ============================================================================

// Helper to wrap Task<T> in SpawnedTask<T>
template <typename T>
SpawnedTask<T> WrapForSpawn(Task<T> task) {
    co_return co_await std::move(task);
}

// Spawn with executor
template <typename T>
SpawnHandle<T> Spawn(Executor& executor, Task<T> task) {
    auto state = std::make_shared<SpawnState<T>>();
    
    auto spawned = WrapForSpawn(std::move(task));
    spawned.SetState(state);
    
    executor.Schedule(spawned.GetHandle());
    
    return SpawnHandle<T>(state);
}

// Spawn using current executor
// Returns a SpawnHandle with a pre-set error if no executor is available.
template <typename T>
SpawnHandle<T> Spawn(Task<T> task) {
    auto* executor = GetCurrentExecutor();
    if (!executor) {
        auto state = std::make_shared<SpawnState<T>>();
        state->Fail(make_error_code(Errc::NoExecutor));
        return SpawnHandle<T>(state);
    }
    return Spawn(*executor, std::move(task));
}

}  // namespace hotcoco
