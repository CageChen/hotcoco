// ============================================================================
// hotcoco/io/polling_executor.hpp - Generic Polling-Based Executor
// ============================================================================
//
// PollingExecutor is a generic Executor that accepts a user-provided
// CompletionSource for polling completions. This enables integration with:
// - RDMA (poll completion queue)
// - DPDK (poll ring buffers)
// - SPDK (poll NVMe queues)
// - Custom hardware with polling interfaces
//
// DESIGN:
// -------
// 1. CompletionSource: User implements Poll() to return coroutine handles
// 2. eventfd: Cross-thread wakeup for Schedule() from other threads
// 3. timerfd: Kernel-level timers for ScheduleAfter()
//
// USAGE:
// ------
//   class MyCQSource : public CompletionSource {
//       size_t Poll(std::span<std::coroutine_handle<>> out) override {
//           // Poll your hardware, return handles
//       }
//   };
//
//   PollingExecutor::Options opts;
//   opts.cpu_affinity = CpuAffinity::SingleCore(0);  // Pin to core 0
//   opts.thread_name = "my-poller";
//   PollingExecutor executor(std::make_unique<MyCQSource>(), opts);
//   executor.Run();
//
// ============================================================================

#pragma once

#include "hotcoco/core/error.hpp"
#include "hotcoco/core/result.hpp"
#include "hotcoco/io/executor.hpp"
#include "hotcoco/io/thread_utils.hpp"

#include <atomic>
#include <chrono>
#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <span>
#include <vector>

namespace hotcoco {

// ============================================================================
// CompletionSource - Abstract Interface for Polling Completions
// ============================================================================
//
// Users implement this interface to provide coroutine handles from their
// specific completion mechanism (RDMA CQ, DPDK ring, etc.).
//
class CompletionSource {
   public:
    virtual ~CompletionSource() = default;

    // Poll for completions. Write up to out.size() handles to `out`.
    // Returns the number of handles written.
    //
    // This is called in a loop by PollingExecutor::Run().
    // Return 0 if no completions are available.
    virtual size_t Poll(std::span<std::coroutine_handle<>> out) = 0;

    // Optional: Block until completions are available or timeout expires.
    // Return true if completions may be available, false on timeout.
    //
    // Default implementation returns immediately (busy-poll mode).
    // Override for event-driven mode (e.g., ibv_get_cq_event).
    virtual bool Wait(std::chrono::milliseconds timeout) {
        (void)timeout;
        return false;
    }
};

// ============================================================================
// PollingExecutor - Generic Polling-Based Executor
// ============================================================================
class PollingExecutor : public Executor {
   public:
    // Configuration options
    struct Options {
        // Idle strategy when no completions
        enum class IdleStrategy {
            Spin,   // Busy-poll (lowest latency, highest CPU)
            Sleep,  // Sleep briefly between polls
            Wait,   // Call CompletionSource::Wait()
        };

        // Maximum completions to poll per iteration
        size_t batch_size = 32;

        IdleStrategy idle_strategy = IdleStrategy::Spin;

        // Sleep duration for IdleStrategy::Sleep
        std::chrono::microseconds sleep_duration{100};

        // ====================================================================
        // Thread Configuration
        // ====================================================================

        // CPU affinity for the polling thread
        CpuAffinity cpu_affinity;

        // Thread name (visible in htop, top, etc.)
        std::string thread_name;

        // Nice value (-20 highest priority, 19 lowest)
        int nice_value = 0;

        Options() = default;
    };

    // Static factory — returns Result instead of throwing
    static Result<std::unique_ptr<PollingExecutor>, std::error_code> Create(std::unique_ptr<CompletionSource> source);
    static Result<std::unique_ptr<PollingExecutor>, std::error_code> Create(std::unique_ptr<CompletionSource> source,
                                                                            Options options);

    ~PollingExecutor() override;

    // Non-copyable, non-movable
    PollingExecutor(const PollingExecutor&) = delete;
    PollingExecutor& operator=(const PollingExecutor&) = delete;

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

   private:
    // Private constructor used by Create()
    PollingExecutor(std::unique_ptr<CompletionSource> source, Options options, int eventfd, int timerfd);

    // ========================================================================
    // Internal Types
    // ========================================================================
    struct TimerEntry {
        std::chrono::steady_clock::time_point deadline;
        std::coroutine_handle<> handle;

        bool operator>(const TimerEntry& other) const { return deadline > other.deadline; }
    };

    // ========================================================================
    // Internal Methods
    // ========================================================================
    void ProcessReadyQueue();
    void ProcessTimers();
    void WakeUp();  // Signal eventfd

    // ========================================================================
    // State
    // ========================================================================
    std::unique_ptr<CompletionSource> source_;
    Options options_;

    int eventfd_ = -1;  // For cross-thread wakeup
    int timerfd_ = -1;  // For timer support

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // Ready queue (protected by mutex for thread-safety)
    std::mutex queue_mutex_;
    std::queue<std::coroutine_handle<>> ready_queue_;
    std::queue<std::function<void()>> callback_queue_;

    // Timer heap (min-heap by deadline)
    std::priority_queue<TimerEntry, std::vector<TimerEntry>, std::greater<TimerEntry>> timer_heap_;
};

}  // namespace hotcoco
