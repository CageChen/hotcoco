// ============================================================================
// ThreadPoolExecutor Implementation
// ============================================================================

#include "hotcoco/io/thread_pool_executor.hpp"

#include <algorithm>

namespace hotcoco {

// ============================================================================
// Construction / Destruction
// ============================================================================

ThreadPoolExecutor::ThreadPoolExecutor() : ThreadPoolExecutor(Options{}) {}

ThreadPoolExecutor::ThreadPoolExecutor(size_t num_threads) {
    options_.num_threads = num_threads;
    if (options_.num_threads == 0) {
        options_.num_threads = 1;
    }
    InitWorkers();
}

ThreadPoolExecutor::ThreadPoolExecutor(const Options& options)
    : options_(options) {
    if (options_.num_threads == 0) {
        options_.num_threads = 1;
    }
    InitWorkers();
}

void ThreadPoolExecutor::InitWorkers() {
    running_ = true;
    
    // Start worker threads
    workers_.reserve(options_.num_threads);
    for (size_t i = 0; i < options_.num_threads; ++i) {
        workers_.emplace_back([this, i] { WorkerLoop(i); });
    }
    
    // Start timer thread
    timer_thread_ = std::thread([this] { TimerLoop(); });
}

ThreadPoolExecutor::~ThreadPoolExecutor() {
    Stop();

    // Wake up all workers
    work_available_.notify_all();
    delayed_cv_.notify_all();

    // Join workers
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    // Join timer
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }

    // Do NOT destroy coroutine handles — they are owned by their
    // respective coroutine types (SpawnedTask, DetachedTask, etc.)
    // Queues are cleared by their destructors automatically.
}

// ============================================================================
// Executor Interface
// ============================================================================

void ThreadPoolExecutor::Run() {
    running_ = true;
    stopping_ = false;
    
    // Wait until stopped or no more work
    std::unique_lock<std::mutex> lock(queue_mutex_);
    work_available_.wait(lock, [this] {
        return stopping_ || 
               (work_queue_.empty() && active_tasks_ == 0);
    });
}

void ThreadPoolExecutor::RunOnce() {
    WorkItem item{std::coroutine_handle<>{nullptr}};
    
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (work_queue_.empty()) {
            return;
        }
        item = std::move(work_queue_.front());
        work_queue_.pop();
    }
    
    // Execute the item
    if (item.handle) {
        active_tasks_++;
        item.handle.resume();
        active_tasks_--;
    } else if (item.callback) {
        active_tasks_++;
        item.callback();
        active_tasks_--;
    }
}

void ThreadPoolExecutor::Stop() {
    stopping_ = true;
    running_ = false;
    work_available_.notify_all();
    delayed_cv_.notify_all();
}

bool ThreadPoolExecutor::IsRunning() const {
    return running_;
}

void ThreadPoolExecutor::Schedule(std::coroutine_handle<> handle) {
    if (!handle) return;
    
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        work_queue_.emplace(handle);
    }
    work_available_.notify_one();
}

void ThreadPoolExecutor::ScheduleAfter(std::chrono::milliseconds delay,
                                        std::coroutine_handle<> handle) {
    if (!handle) return;
    
    auto when = std::chrono::steady_clock::now() + delay;
    
    {
        std::lock_guard<std::mutex> lock(delayed_mutex_);
        delayed_queue_.push({when, handle});
    }
    delayed_cv_.notify_one();
}

void ThreadPoolExecutor::Post(std::function<void()> callback) {
    if (!callback) return;
    
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        work_queue_.emplace(std::move(callback));
    }
    work_available_.notify_one();
}

size_t ThreadPoolExecutor::PendingTasks() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return work_queue_.size();
}

// ============================================================================
// Worker Thread
// ============================================================================

void ThreadPoolExecutor::WorkerLoop(size_t worker_index) {
    // Set this executor as current for this thread
    SetCurrentExecutor(this);

    // Apply thread configuration
    if (options_.cpu_affinity.IsSet()) {
        // Round-robin assignment: worker[i] -> cpus[i % cpus.size()]
        int cpu = options_.cpu_affinity.cpus[worker_index % options_.cpu_affinity.cpus.size()];
        SetThreadAffinity(CpuAffinity::SingleCore(cpu));
    }
    if (!options_.thread_name_prefix.empty()) {
        SetThreadName(options_.thread_name_prefix + "-" + std::to_string(worker_index));
    }
    if (options_.nice_value != 0) {
        SetThreadNice(options_.nice_value);
    }
    
    while (!stopping_) {
        WorkItem item{std::coroutine_handle<>{nullptr}};
        
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            work_available_.wait(lock, [this] {
                return stopping_ || !work_queue_.empty();
            });
            
            if (stopping_ && work_queue_.empty()) {
                break;
            }
            
            if (!work_queue_.empty()) {
                item = std::move(work_queue_.front());
                work_queue_.pop();
                // Increment active_tasks_ while still holding the lock so that
                // Run() never sees empty queue + zero active tasks prematurely.
                active_tasks_++;
            }
        }

        // Execute outside the lock
        if (item.handle) {
            item.handle.resume();
            active_tasks_--;

            // Notify in case Run() is waiting
            work_available_.notify_all();
        } else if (item.callback) {
            item.callback();
            active_tasks_--;

            work_available_.notify_all();
        }
    }
    
    SetCurrentExecutor(nullptr);
}

// ============================================================================
// Timer Thread
// ============================================================================

void ThreadPoolExecutor::TimerLoop() {
    while (!stopping_) {
        std::coroutine_handle<> handle{nullptr};
        
        {
            std::unique_lock<std::mutex> lock(delayed_mutex_);
            
            if (delayed_queue_.empty()) {
                // Wait for new delayed work or stop
                delayed_cv_.wait(lock, [this] {
                    return stopping_ || !delayed_queue_.empty();
                });
                continue;
            }
            
            // Wait until soonest item is ready
            auto& top = delayed_queue_.top();
            if (delayed_cv_.wait_until(lock, top.when, [this] {
                return stopping_.load();
            })) {
                // Stopping
                break;
            }
            
            // Check if it's time
            if (std::chrono::steady_clock::now() >= top.when) {
                handle = top.handle;
                delayed_queue_.pop();
            }
        }
        
        // Schedule the handle on the work queue
        if (handle) {
            Schedule(handle);
        }
    }
}

}  // namespace hotcoco
