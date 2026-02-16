// ============================================================================
// hotcoco/io/iouring_executor.hpp - io_uring-based Event Loop
// ============================================================================
//
// IoUringExecutor is a high-performance Executor implementation using Linux's
// io_uring interface. It provides significantly lower overhead than epoll-based
// solutions like libuv for I/O-heavy workloads.
//
// WHY IO_URING:
// -------------
// 1. Batched system calls - submit many operations with one syscall
// 2. True async - no callback overhead, direct completion queue polling
// 3. Zero-copy capable - for advanced use cases
// 4. Linux 5.1+ only - modern kernels required
//
// ARCHITECTURE:
// -------------
// - io_uring ring: Submission and completion queues
// - eventfd: For thread-safe wakeup from other threads
// - IORING_OP_TIMEOUT: For delayed coroutine scheduling
//
// USAGE:
// ------
//   #ifdef HOTCOCO_HAS_IOURING
//   IoUringExecutor executor;
//   executor.Schedule(some_coroutine_handle);
//   executor.Run();
//   #endif
//
// ============================================================================

#pragma once

#ifdef HOTCOCO_HAS_IOURING

#include "hotcoco/core/error.hpp"
#include "hotcoco/core/result.hpp"
#include "hotcoco/io/executor.hpp"

#include <atomic>
#include <chrono>
#include <coroutine>
#include <functional>
#include <liburing.h>
#include <memory>
#include <mutex>
#include <queue>
#include <system_error>
#include <unordered_set>

namespace hotcoco {

// ============================================================================
// IoUringExecutor - io_uring-based Executor Implementation
// ============================================================================
class IoUringExecutor : public Executor {
   public:
    struct Config {
        uint32_t queue_depth;
        bool sqpoll;
        bool provided_buffers;
        uint32_t buffer_ring_size;
        uint32_t buffer_size;
        uint16_t buffer_group_id;

        Config()
            : queue_depth(256),
              sqpoll(false),
              provided_buffers(false),
              buffer_ring_size(1024),
              buffer_size(4096),
              buffer_group_id(0) {}
    };

    // Static factory — returns Result instead of throwing
    [[nodiscard]] static Result<std::unique_ptr<IoUringExecutor>, std::error_code> Create(
        const Config& config = Config());
    ~IoUringExecutor() override;

    // Non-copyable, non-movable (io_uring state can't be moved)
    IoUringExecutor(const IoUringExecutor&) = delete;
    IoUringExecutor& operator=(const IoUringExecutor&) = delete;

    // ========================================================================
    // Executor Interface Implementation
    // ========================================================================

    void Run() override;
    void RunOnce() override;
    void Stop() override;
    [[nodiscard]] bool IsRunning() const override;

    void Schedule(std::coroutine_handle<> handle) override;
    void ScheduleAfter(std::chrono::milliseconds delay, std::coroutine_handle<> handle) override;
    void Post(std::function<void()> callback) override;

    // Access the underlying io_uring ring (for TCP, etc.)
    [[nodiscard]] struct io_uring* GetRing() { return &ring_; }

    [[nodiscard]] const Config& GetConfig() const { return config_; }

    // Provide a buffer slot back to the ring
    void ReturnBuffer(uint16_t bgid, uint16_t bid);

    // Get the base block of the allocated buffer array
    [[nodiscard]] char* GetBufferBase() const { return buf_ring_data_; }

   public:
    // ========================================================================
    // Types (public for use by IoUringXxx() awaitables)
    // ========================================================================

    // Operation types for completion queue entries
    enum class OpType : uint8_t {
        Wakeup,   // eventfd read completion
        Timeout,  // Timer completion
        IO,       // Generic I/O (TCP accept/connect/recv/send, etc.)
    };

    // Context for completion queue entries
    struct OpContext {
        OpType type;
        std::coroutine_handle<> handle;
        int32_t result = 0;      // CQE result for IO ops
        uint32_t cqe_flags = 0;  // CQE flags for extra info (e.g., buffer selection)
        __kernel_timespec ts{};  // Owned inline — no separate allocation
    };

   private:
    // Timer request for thread-safe ScheduleAfter
    struct TimerRequest {
        std::chrono::milliseconds delay;
        std::coroutine_handle<> handle;
    };

    // ========================================================================
    // Internal Methods
    // ========================================================================

    // Process completions from the io_uring CQ
    void ProcessCompletions();

    // Process ready queue of coroutines and callbacks
    void ProcessReadyQueue();

    // Submit a wakeup read on the eventfd
    void SubmitWakeupRead();

    // ========================================================================
    // State
    // ========================================================================
    Config config_;
    struct io_uring_buf_ring* buf_ring_ = nullptr;
    char* buf_ring_data_ = nullptr;

    struct io_uring ring_;
    int eventfd_ = -1;  // For cross-thread wakeup

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // Queue of coroutines ready to run (protected by mutex for thread-safety)
    std::mutex queue_mutex_;
    std::queue<std::coroutine_handle<>> ready_queue_;
    std::queue<std::function<void()>> callback_queue_;
    std::queue<TimerRequest> timer_queue_;

    // Eventfd read buffer
    uint64_t eventfd_buf_ = 0;

    // Context for the wakeup operation (reused)
    OpContext wakeup_ctx_{OpType::Wakeup, nullptr, 0, 0, {}};

    // Track heap-allocated OpContexts (Timeout) so the destructor can free
    // any that are still in-flight in the kernel when the executor is destroyed.
    std::unordered_set<OpContext*> pending_timeouts_;

    // Private constructor used by Create()
    IoUringExecutor(struct io_uring ring, int eventfd, Config config);

    // Setup provided buffer ring
    Result<void, std::error_code> SetupBufferRing();
};

}  // namespace hotcoco

#endif  // HOTCOCO_HAS_IOURING
