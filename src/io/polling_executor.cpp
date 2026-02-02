// ============================================================================
// hotcoco/io/polling_executor.cpp - Generic Polling-Based Executor
// ============================================================================

#include "hotcoco/io/polling_executor.hpp"

#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <thread>

namespace hotcoco {

// ============================================================================
// Constructor / Destructor
// ============================================================================

PollingExecutor::PollingExecutor(std::unique_ptr<CompletionSource> source,
                                 Options options, int eventfd, int timerfd)
    : source_(std::move(source)),
      options_(options),
      eventfd_(eventfd),
      timerfd_(timerfd) {}

Result<std::unique_ptr<PollingExecutor>, std::error_code> PollingExecutor::Create(
    std::unique_ptr<CompletionSource> source) {
    return Create(std::move(source), Options{});
}

Result<std::unique_ptr<PollingExecutor>, std::error_code> PollingExecutor::Create(
    std::unique_ptr<CompletionSource> source, Options options) {
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) {
        return Err(make_error_code(Errc::EventfdFailed));
    }

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) {
        close(efd);
        return Err(make_error_code(Errc::TimerfdFailed));
    }

    return Ok(std::unique_ptr<PollingExecutor>(
        new PollingExecutor(std::move(source), options, efd, tfd)));
}

PollingExecutor::~PollingExecutor() {
    if (eventfd_ >= 0) {
        close(eventfd_);
    }
    if (timerfd_ >= 0) {
        close(timerfd_);
    }
}

// ============================================================================
// Event Loop Control
// ============================================================================

void PollingExecutor::Run() {
    ExecutorGuard guard(this);
    running_ = true;
    stop_requested_ = false;

    // Apply thread configuration
    if (options_.cpu_affinity.IsSet()) {
        SetThreadAffinity(options_.cpu_affinity);
    }
    if (!options_.thread_name.empty()) {
        SetThreadName(options_.thread_name);
    }
    if (options_.nice_value != 0) {
        SetThreadNice(options_.nice_value);
    }

    std::vector<std::coroutine_handle<>> completion_buffer(options_.batch_size);

    while (!stop_requested_) {
        // 1. Poll for completions from user's source
        size_t n = source_->Poll(completion_buffer);
        for (size_t i = 0; i < n; ++i) {
            if (completion_buffer[i]) {
                completion_buffer[i].resume();
            }
        }

        // 2. Process timers that have expired
        ProcessTimers();

        // 3. Process ready queue (scheduled coroutines + callbacks)
        ProcessReadyQueue();

        // 4. Drain eventfd (consume wakeup signal)
        uint64_t val;
        [[maybe_unused]] auto ret = read(eventfd_, &val, sizeof(val));

        // 5. Handle idle (no completions)
        bool queues_empty;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queues_empty = ready_queue_.empty() && callback_queue_.empty();
        }
        if (n == 0 && queues_empty) {
            switch (options_.idle_strategy) {
                case Options::IdleStrategy::Spin:
                    // Busy poll - do nothing
                    break;
                case Options::IdleStrategy::Sleep:
                    std::this_thread::sleep_for(options_.sleep_duration);
                    break;
                case Options::IdleStrategy::Wait:
                    source_->Wait(std::chrono::milliseconds(1));
                    break;
            }
        }
    }

    running_ = false;
}

void PollingExecutor::RunOnce() {
    ExecutorGuard guard(this);

    std::vector<std::coroutine_handle<>> completion_buffer(options_.batch_size);

    // Poll for completions
    size_t n = source_->Poll(completion_buffer);
    for (size_t i = 0; i < n; ++i) {
        if (completion_buffer[i]) {
            completion_buffer[i].resume();
        }
    }

    // Process timers and ready queue
    ProcessTimers();
    ProcessReadyQueue();

    // Drain eventfd
    uint64_t val;
    [[maybe_unused]] auto ret = read(eventfd_, &val, sizeof(val));
}

void PollingExecutor::Stop() {
    stop_requested_ = true;
    WakeUp();
}

bool PollingExecutor::IsRunning() const {
    return running_;
}

// ============================================================================
// Coroutine Scheduling
// ============================================================================

void PollingExecutor::Schedule(std::coroutine_handle<> handle) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        ready_queue_.push(handle);
    }
    WakeUp();
}

void PollingExecutor::ScheduleAfter(std::chrono::milliseconds delay,
                                    std::coroutine_handle<> handle) {
    auto deadline = std::chrono::steady_clock::now() + delay;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        timer_heap_.push(TimerEntry{deadline, handle});
    }

    // Arm timerfd to fire at the earliest deadline
    // For simplicity, we check timers on each loop iteration
    // A more sophisticated impl could rearm timerfd here
    WakeUp();
}

void PollingExecutor::Post(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        callback_queue_.push(std::move(callback));
    }
    WakeUp();
}

// ============================================================================
// Internal Methods
// ============================================================================

void PollingExecutor::ProcessReadyQueue() {
    std::queue<std::coroutine_handle<>> ready_copy;
    std::queue<std::function<void()>> callback_copy;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::swap(ready_copy, ready_queue_);
        std::swap(callback_copy, callback_queue_);
    }

    // Resume coroutines
    while (!ready_copy.empty()) {
        auto handle = ready_copy.front();
        ready_copy.pop();
        if (handle && !handle.done()) {
            handle.resume();
        }
    }

    // Execute callbacks
    while (!callback_copy.empty()) {
        auto cb = std::move(callback_copy.front());
        callback_copy.pop();
        cb();
    }
}

void PollingExecutor::ProcessTimers() {
    auto now = std::chrono::steady_clock::now();

    std::vector<std::coroutine_handle<>> expired;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);

        while (!timer_heap_.empty() && timer_heap_.top().deadline <= now) {
            auto entry = timer_heap_.top();
            timer_heap_.pop();

            if (entry.handle && !entry.handle.done()) {
                expired.push_back(entry.handle);
            }
        }
    }

    // Resume outside lock to avoid deadlock if coroutine calls Schedule()
    for (auto handle : expired) {
        handle.resume();
    }
}

void PollingExecutor::WakeUp() {
    uint64_t val = 1;
    [[maybe_unused]] auto ret = write(eventfd_, &val, sizeof(val));
}

}  // namespace hotcoco
