// ============================================================================
// hotcoco/io/thread_pool_executor.hpp - Multi-Threaded Task Executor
// ============================================================================
//
// ThreadPoolExecutor runs coroutines across multiple worker threads, enabling
// true parallelism for CPU-bound work and concurrent I/O operations.
//
// KEY CONCEPTS:
// -------------
// 1. WORKER THREADS: A fixed number of threads that pull work from a shared queue
// 2. THREAD-SAFE QUEUE: Lock-free or mutex-protected task queue
// 3. WORK DISTRIBUTION: Tasks are distributed across workers
// 4. COROUTINE SAFETY: Each task runs to completion on one thread
//
// USAGE:
// ------
//   ThreadPoolExecutor::Options opts;
//   opts.num_threads = 4;
//   opts.cpu_affinity = CpuAffinity::Range(0, 3);
//   opts.thread_name_prefix = "worker";
//   ThreadPoolExecutor executor(opts);
//
//   executor.Schedule(task1.GetHandle());
//   executor.Run();
//
// ============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "hotcoco/io/executor.hpp"
#include "hotcoco/io/thread_utils.hpp"

namespace hotcoco {

// ============================================================================
// ThreadPoolExecutor
// ============================================================================
class ThreadPoolExecutor : public Executor {
public:
    // ========================================================================
    // Options
    // ========================================================================
    struct Options {
        // Number of worker threads (default: hardware_concurrency)
        size_t num_threads = std::thread::hardware_concurrency();

        // CPU affinity for worker threads
        // Workers are assigned round-robin: worker[i] -> cpus[i % cpus.size()]
        CpuAffinity cpu_affinity;

        // Thread name prefix (workers named "prefix-0", "prefix-1", etc.)
        std::string thread_name_prefix = "hotcoco-worker";

        // Nice value for worker threads
        int nice_value = 0;

        Options() = default;
    };

    // ========================================================================
    // Construction
    // ========================================================================

    // Create with default options
    ThreadPoolExecutor();

    // Create with specified number of threads (convenience)
    explicit ThreadPoolExecutor(size_t num_threads);

    // Create with full options
    explicit ThreadPoolExecutor(const Options& options);

    ~ThreadPoolExecutor() override;

    // Non-copyable, non-movable
    ThreadPoolExecutor(const ThreadPoolExecutor&) = delete;
    ThreadPoolExecutor& operator=(const ThreadPoolExecutor&) = delete;
    
    // ========================================================================
    // Executor Interface Implementation
    // ========================================================================
    
    // Run until Stop() is called or no more work
    void Run() override;
    
    // Run one iteration (process one task if available)
    void RunOnce() override;
    
    // Signal all workers to stop
    void Stop() override;
    
    // Check if running
    bool IsRunning() const override;
    
    // Schedule a coroutine to run on a worker thread
    void Schedule(std::coroutine_handle<> handle) override;
    
    // Schedule a coroutine to run after a delay
    void ScheduleAfter(std::chrono::milliseconds delay,
                       std::coroutine_handle<> handle) override;
    
    // Post a callback to run on a worker thread
    void Post(std::function<void()> callback) override;
    
    // ========================================================================
    // Thread Pool Specific
    // ========================================================================
    
    // Get the number of worker threads
    size_t NumThreads() const { return workers_.size(); }
    
    // Get the number of pending tasks
    size_t PendingTasks() const;
    
private:
    // Work item: either a coroutine handle or a callback
    struct WorkItem {
        std::coroutine_handle<> handle{nullptr};
        std::function<void()> callback;
        
        explicit WorkItem(std::coroutine_handle<> h) : handle(h) {}
        explicit WorkItem(std::function<void()> cb) : callback(std::move(cb)) {}
    };
    
    // Delayed work item
    struct DelayedWork {
        std::chrono::steady_clock::time_point when;
        std::coroutine_handle<> handle;
        
        bool operator>(const DelayedWork& other) const {
            return when > other.when;
        }
    };
    
    // Worker thread function (index for affinity assignment)
    void WorkerLoop(size_t worker_index);
    
    // Timer thread function (handles delayed work)
    void TimerLoop();
    
    // Internal: get next work item (blocks if queue is empty)
    bool TryGetWork(WorkItem& item);

    // Internal: initialize worker threads
    void InitWorkers();
    
    // Configuration
    Options options_;

    // Worker threads
    std::vector<std::thread> workers_;
    
    // Timer thread for delayed execution
    std::thread timer_thread_;
    
    // Work queue
    std::queue<WorkItem> work_queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable work_available_;
    
    // Delayed work (priority queue, earliest first)
    std::priority_queue<DelayedWork, std::vector<DelayedWork>,
                        std::greater<DelayedWork>> delayed_queue_;
    std::mutex delayed_mutex_;
    std::condition_variable delayed_cv_;
    
    // State
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<size_t> active_tasks_{0};
};

}  // namespace hotcoco
