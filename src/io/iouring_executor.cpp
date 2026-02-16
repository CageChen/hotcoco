// ============================================================================
// hotcoco/io/iouring_executor.cpp - io_uring-based Event Loop Implementation
// ============================================================================

#ifdef HOTCOCO_HAS_IOURING

#include "hotcoco/io/iouring_executor.hpp"

#include <sys/eventfd.h>

#include <cassert>
#include <cstring>
#include <unistd.h>

namespace hotcoco {

// ============================================================================
// Construction / Destruction
// ============================================================================

IoUringExecutor::IoUringExecutor(struct io_uring ring, int eventfd, Config config)
    : config_(config), ring_(ring), eventfd_(eventfd) {
    if (config_.provided_buffers) {
        auto res = SetupBufferRing();
        if (res.IsErr()) {
            config_.provided_buffers = false;  // Fallback
        }
    }
    SubmitWakeupRead();
    io_uring_submit(&ring_);  // Ensure the wakeup read SQE is registered with the kernel immediately
}

Result<std::unique_ptr<IoUringExecutor>, std::error_code> IoUringExecutor::Create(const Config& config) {
    struct io_uring ring{};
    struct io_uring_params params{};
    if (config.sqpoll) {
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = 2000;
    }

    int ret = io_uring_queue_init_params(config.queue_depth, &ring, &params);
    if (ret < 0) {
        // Fallback if SQPOLL is requested but not permitted (e.g. without CAP_SYS_ADMIN/sysctls)
        if (config.sqpoll && ret == -EPERM) {
            params.flags &= ~IORING_SETUP_SQPOLL;
            ret = io_uring_queue_init_params(config.queue_depth, &ring, &params);
        }
        if (ret < 0) {
            return Err(make_error_code(Errc::IoUringInitFailed));
        }
    }

    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) {
        io_uring_queue_exit(&ring);
        return Err(make_error_code(Errc::EventfdFailed));
    }

    return Ok(std::unique_ptr<IoUringExecutor>(new IoUringExecutor(ring, efd, config)));
}

IoUringExecutor::~IoUringExecutor() {
    if (running_) {
        Stop();
    }

    // Drain any CQEs that arrived between the last ProcessCompletions() and now
    struct io_uring_cqe* cqe = nullptr;
    unsigned head = 0;
    unsigned count = 0;
    io_uring_for_each_cqe(&ring_, head, cqe) {
        ++count;
        auto* ctx = static_cast<OpContext*>(io_uring_cqe_get_data(cqe));
        if (ctx && ctx != &wakeup_ctx_ && ctx->type == OpType::Timeout) {
            pending_timeouts_.erase(ctx);
            delete ctx;
        }
    }
    if (count > 0) {
        io_uring_cq_advance(&ring_, count);
    }

    // Delete any remaining in-flight timeout OpContexts that the kernel
    // hasn't completed yet (e.g. timers submitted but not yet fired)
    for (auto* ctx : pending_timeouts_) {
        delete ctx;
    }
    pending_timeouts_.clear();

    if (buf_ring_) {
        io_uring_unregister_buf_ring(&ring_, config_.buffer_group_id);
        io_uring_free_buf_ring(&ring_, buf_ring_, config_.buffer_ring_size, config_.buffer_group_id);
        buf_ring_ = nullptr;
    }

    if (buf_ring_data_) {
        // buf_ring_data is typically allocated by posix_memalign
        free(buf_ring_data_);
        buf_ring_data_ = nullptr;
    }

    if (eventfd_ >= 0) {
        close(eventfd_);
    }

    io_uring_queue_exit(&ring_);
}

// ============================================================================
// Buffer Ring Setup
// ============================================================================

Result<void, std::error_code> IoUringExecutor::SetupBufferRing() {
    // 1. Setup the buffer ring via liburing
    int ret = 0;
    buf_ring_ = io_uring_setup_buf_ring(&ring_, config_.buffer_ring_size, config_.buffer_group_id, 0, &ret);
    if (!buf_ring_) {
        return Err(make_error_code(Errc::IoUringInitFailed));  // Needs newer kernel
    }

    // 2. Allocate the actual data buffers (page-aligned)
    size_t total_size = static_cast<size_t>(config_.buffer_ring_size) * config_.buffer_size;
    if (posix_memalign(reinterpret_cast<void**>(&buf_ring_data_), 4096, total_size) != 0) {
        io_uring_free_buf_ring(&ring_, buf_ring_, config_.buffer_ring_size, config_.buffer_group_id);
        buf_ring_ = nullptr;
        return Err(make_error_code(Errc::IoUringInitFailed));
    }

    // 3. Populate the ring with all buffers initially
    io_uring_buf_ring_init(buf_ring_);
    for (uint16_t i = 0; i < config_.buffer_ring_size; ++i) {
        char* buf_ptr = buf_ring_data_ + (i * config_.buffer_size);
        io_uring_buf_ring_add(buf_ring_, buf_ptr, config_.buffer_size, i,
                              io_uring_buf_ring_mask(config_.buffer_ring_size), i);
    }
    io_uring_buf_ring_advance(buf_ring_, config_.buffer_ring_size);

    return Ok();
}

void IoUringExecutor::ReturnBuffer(uint16_t bgid, uint16_t bid) {
    if (!buf_ring_ || bgid != config_.buffer_group_id || bid >= config_.buffer_ring_size) {
        return;
    }
    char* buf_ptr = buf_ring_data_ + (bid * config_.buffer_size);
    io_uring_buf_ring_add(buf_ring_, buf_ptr, config_.buffer_size, bid,
                          io_uring_buf_ring_mask(config_.buffer_ring_size), 0);
    io_uring_buf_ring_advance(buf_ring_, 1);
}

// ============================================================================
// Event Loop Control
// ============================================================================

void IoUringExecutor::Run() {
    running_ = true;
    stop_requested_ = false;

    // Set this as the current thread's executor
    ExecutorGuard guard(this);

    while (!stop_requested_) {
        // Submit any pending SQEs and wait for at least one completion
        struct io_uring_cqe* cqe;
        int ret = io_uring_submit_and_wait(&ring_, 1);
        if (ret < 0) {
            ret = io_uring_wait_cqe(&ring_, &cqe);
        } else {
            ret = io_uring_peek_cqe(&ring_, &cqe);
            if (ret == -EAGAIN) {
                ret = io_uring_wait_cqe(&ring_, &cqe);
            }
        }

        if (ret < 0) {
            if (ret == -EINTR) {
                continue;  // Interrupted, retry
            }
            break;  // Fatal error
        }

        // Process all available completions
        ProcessCompletions();

        // Process ready queue
        ProcessReadyQueue();
    }

    running_ = false;
}

void IoUringExecutor::RunOnce() {
    ExecutorGuard guard(this);

    // Submit pending SQEs and peek for completions non-blocking
    io_uring_submit(&ring_);

    struct io_uring_cqe* cqe;
    unsigned head;
    unsigned count = 0;

    io_uring_for_each_cqe(&ring_, head, cqe) {
        count++;
    }

    if (count > 0) {
        ProcessCompletions();
    }

    ProcessReadyQueue();
}

void IoUringExecutor::Stop() {
    stop_requested_ = true;

    // Wake up the event loop if it's waiting
    uint64_t val = 1;
    [[maybe_unused]] ssize_t n = write(eventfd_, &val, sizeof(val));
}

bool IoUringExecutor::IsRunning() const {
    return running_;
}

// ============================================================================
// Coroutine Scheduling
// ============================================================================

void IoUringExecutor::Schedule(std::coroutine_handle<> handle) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        ready_queue_.push(handle);
    }

    // Wake up the event loop (thread-safe via eventfd)
    uint64_t val = 1;
    [[maybe_unused]] ssize_t n = write(eventfd_, &val, sizeof(val));
}

void IoUringExecutor::ScheduleAfter(std::chrono::milliseconds delay, std::coroutine_handle<> handle) {
    // Queue the timer request for processing on the event loop thread.
    // io_uring_get_sqe/io_uring_submit are NOT thread-safe, so we must
    // create timers only from the loop thread.
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        timer_queue_.push({delay, handle});
    }

    // Wake up the event loop (thread-safe via eventfd)
    uint64_t val = 1;
    [[maybe_unused]] ssize_t n = write(eventfd_, &val, sizeof(val));
}

void IoUringExecutor::Post(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        callback_queue_.push(std::move(callback));
    }

    // Wake up the event loop
    uint64_t val = 1;
    [[maybe_unused]] ssize_t n = write(eventfd_, &val, sizeof(val));
}

// ============================================================================
// Internal Methods
// ============================================================================

void IoUringExecutor::ProcessCompletions() {
    struct io_uring_cqe* cqe;
    unsigned head;
    unsigned count = 0;

    io_uring_for_each_cqe(&ring_, head, cqe) {
        ++count;
        auto* ctx = static_cast<OpContext*>(io_uring_cqe_get_data(cqe));

        if (ctx == &wakeup_ctx_) {
            // Wakeup event - drain the eventfd and re-arm
            // The buffer was already read by the kernel into eventfd_buf_
            SubmitWakeupRead();
        } else if (ctx != nullptr) {
            switch (ctx->type) {
                case OpType::Timeout: {
                    // Timer fired - resume the coroutine
                    // Note: res == -ETIME is the normal timeout completion
                    pending_timeouts_.erase(ctx);
                    if (ctx->handle) {
                        ctx->handle.resume();
                    }
                    delete ctx;
                    break;
                }
                case OpType::IO: {
                    // Generic I/O completion — store result and resume
                    // OpContext lives on the coroutine frame, NOT deleted here
                    ctx->result = cqe->res;
                    ctx->cqe_flags = cqe->flags;
                    if (ctx->handle) {
                        ctx->handle.resume();
                    }
                    break;
                }
                case OpType::Wakeup:
                    // Already handled above
                    break;
            }
        }
    }

    // Advance the CQ head by the exact number of entries we processed
    io_uring_cq_advance(&ring_, count);
}

void IoUringExecutor::ProcessReadyQueue() {
    // Swap queues to avoid holding lock while processing
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

    // Create timers on the loop thread (io_uring APIs are NOT thread-safe)
    while (!timers.empty()) {
        auto req = timers.front();
        timers.pop();

        // OpContext owns the __kernel_timespec inline — no separate allocation
        auto* ctx = new OpContext{OpType::Timeout, req.handle, 0, {}};
        ctx->ts.tv_sec = req.delay.count() / 1000;
        ctx->ts.tv_nsec = (req.delay.count() % 1000) * 1000000;

        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (sqe == nullptr) {
            // SQE exhausted — fall back to immediate schedule
            if (ctx->handle) {
                ctx->handle.resume();
            }
            delete ctx;
            continue;
        }

        io_uring_prep_timeout(sqe, &ctx->ts, 0, 0);
        io_uring_sqe_set_data(sqe, ctx);
        pending_timeouts_.insert(ctx);
    }
}

void IoUringExecutor::SubmitWakeupRead() {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
        return;  // SQE exhausted, will retry later
    }

    io_uring_prep_read(sqe, eventfd_, &eventfd_buf_, sizeof(eventfd_buf_), 0);
    io_uring_sqe_set_data(sqe, &wakeup_ctx_);
}

}  // namespace hotcoco

#endif  // HOTCOCO_HAS_IOURING
