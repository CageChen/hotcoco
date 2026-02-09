// ============================================================================
// hotcoco/io/libuv_executor.cpp - libuv-based Event Loop Implementation
// ============================================================================

#include "hotcoco/io/libuv_executor.hpp"

#include "hotcoco/core/check.hpp"

namespace hotcoco {

// ============================================================================
// Construction / Destruction
// ============================================================================

LibuvExecutor::LibuvExecutor() = default;

Result<std::unique_ptr<LibuvExecutor>, std::error_code> LibuvExecutor::Create() {
    auto executor = std::unique_ptr<LibuvExecutor>(new LibuvExecutor());

    // Initialize the libuv event loop
    int result = uv_loop_init(&executor->loop_);
    if (result != 0) return Err(make_error_code(Errc::IoError));

    // Initialize async handle for cross-thread scheduling
    // This allows Schedule() to be called from any thread
    result = uv_async_init(&executor->loop_, &executor->async_, OnAsync);
    if (result != 0) {
        uv_loop_close(&executor->loop_);
        return Err(make_error_code(Errc::IoError));
    }

    // Store 'this' in the handle's data field for callback access
    executor->async_.data = executor.get();

    // Initialize idle handle for processing ready queue
    // The idle callback runs when the loop has nothing else to do
    result = uv_idle_init(&executor->loop_, &executor->idle_);
    if (result != 0) {
        uv_close(reinterpret_cast<uv_handle_t*>(&executor->async_), nullptr);
        uv_run(&executor->loop_, UV_RUN_ONCE);
        uv_loop_close(&executor->loop_);
        return Err(make_error_code(Errc::IoError));
    }
    executor->idle_.data = executor.get();

    return Ok(std::move(executor));
}

LibuvExecutor::~LibuvExecutor() {
    // Stop the loop if still running
    if (running_) {
        Stop();
    }

    // Close handles before destroying the loop
    // We need to run the loop to process close callbacks
    uv_close(reinterpret_cast<uv_handle_t*>(&async_), nullptr);
    uv_close(reinterpret_cast<uv_handle_t*>(&idle_), nullptr);

    // Run until all handles are closed
    while (uv_loop_alive(&loop_)) {
        uv_run(&loop_, UV_RUN_ONCE);
    }

    uv_loop_close(&loop_);
}

// ============================================================================
// Event Loop Control
// ============================================================================

void LibuvExecutor::Run() {
    running_ = true;

    // Set this as the current thread's executor
    ExecutorGuard guard(this);

    // Run the event loop
    // UV_RUN_DEFAULT runs until Stop() is called or no more active handles
    uv_run(&loop_, UV_RUN_DEFAULT);

    running_ = false;
}

void LibuvExecutor::RunOnce() {
    ExecutorGuard guard(this);
    uv_run(&loop_, UV_RUN_NOWAIT);
}

void LibuvExecutor::Stop() {
    uv_stop(&loop_);
    running_ = false;
}

bool LibuvExecutor::IsRunning() const {
    return running_;
}

// ============================================================================
// Coroutine Scheduling
// ============================================================================

void LibuvExecutor::Schedule(std::coroutine_handle<> handle) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        ready_queue_.push(handle);
    }

    // Wake up the event loop if it's waiting
    // uv_async_send is thread-safe
    uv_async_send(&async_);
}

void LibuvExecutor::ScheduleAfter(std::chrono::milliseconds delay, std::coroutine_handle<> handle) {
    // Queue the timer request for processing on the event loop thread.
    // uv_timer_init/uv_timer_start are NOT thread-safe, so we must
    // create timers only from the loop thread.
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        timer_queue_.push({delay, handle});
    }
    uv_async_send(&async_);
}

void LibuvExecutor::Post(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        callback_queue_.push(std::move(callback));
    }
    uv_async_send(&async_);
}

// ============================================================================
// libuv Callbacks
// ============================================================================

void LibuvExecutor::OnAsync(uv_async_t* handle) {
    // The async callback is triggered by uv_async_send
    // We use it to start the idle handle which processes the queue
    auto* self = static_cast<LibuvExecutor*>(handle->data);

    // Start idle handle if not already active
    if (!self->idle_active_) {
        uv_idle_start(&self->idle_, OnIdle);
        self->idle_active_ = true;
    }
}

void LibuvExecutor::OnTimer(uv_timer_t* handle) {
    // Timer fired - resume the coroutine
    auto* scheduled = static_cast<ScheduledHandle*>(handle->data);

    // Resume the coroutine unconditionally. The handle is guaranteed valid
    // by the caller who scheduled it. Checking .done() on a potentially
    // destroyed handle is UB.
    if (scheduled->handle) {
        scheduled->handle.resume();
    }

    // Clean up the timer
    uv_close(reinterpret_cast<uv_handle_t*>(handle), OnClose);
}

void LibuvExecutor::OnIdle(uv_idle_t* handle) {
    auto* self = static_cast<LibuvExecutor*>(handle->data);
    self->ProcessReadyQueue();

    // Check if all queues are empty
    std::lock_guard<std::mutex> lock(self->queue_mutex_);
    if (self->ready_queue_.empty() && self->callback_queue_.empty() && self->timer_queue_.empty()) {
        uv_idle_stop(handle);
        self->idle_active_ = false;
    }
}

void LibuvExecutor::OnClose(uv_handle_t* handle) {
    // Clean up the scheduled handle data
    if (handle->type == UV_TIMER) {
        auto* timer = reinterpret_cast<uv_timer_t*>(handle);
        auto* scheduled = static_cast<ScheduledHandle*>(timer->data);
        delete scheduled;
        delete timer;
    }
}

// ============================================================================
// Internal Helpers
// ============================================================================

void LibuvExecutor::ProcessReadyQueue() {
    // Process all ready coroutines
    // We swap the queue to avoid holding the lock while resuming
    std::queue<std::coroutine_handle<>> ready;
    std::queue<std::function<void()>> callbacks;
    std::queue<TimerRequest> timers;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::swap(ready, ready_queue_);
        std::swap(callbacks, callback_queue_);
        std::swap(timers, timer_queue_);
    }

    // Resume all ready coroutines
    while (!ready.empty()) {
        auto handle = ready.front();
        ready.pop();

        if (handle) {
            handle.resume();
        }
    }

    // Run all callbacks
    while (!callbacks.empty()) {
        auto callback = std::move(callbacks.front());
        callbacks.pop();
        callback();
    }

    // Create timers on the loop thread (uv_timer_init/start are NOT thread-safe)
    while (!timers.empty()) {
        auto req = timers.front();
        timers.pop();

        auto* timer = new uv_timer_t;
        int result = uv_timer_init(&loop_, timer);
        if (result != 0) {
            // Timer init failed — fall back to immediate resume
            delete timer;
            if (req.handle) {
                req.handle.resume();
            }
            continue;
        }

        auto* scheduled = new ScheduledHandle{req.handle, timer};
        timer->data = scheduled;

        uv_timer_start(timer, OnTimer, req.delay.count(), 0);
    }
}

}  // namespace hotcoco
