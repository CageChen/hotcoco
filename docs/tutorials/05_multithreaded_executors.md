# Tutorial 5: Multi-Threaded Executors and Task Composition

This tutorial explores the `ThreadPoolExecutor` for parallel execution and
the `WhenAll`/`WhenAny` combinators for composing multiple tasks.

## What You'll Learn

1. **WHAT**: Thread pool executors and task composition
2. **HOW**: Implementation details of `ThreadPoolExecutor`, `WhenAll`, `WhenAny`
3. **WHY**: Design decisions and trade-offs

---

## Part 1: ThreadPoolExecutor

### 1.1 What is a Thread Pool Executor?

A **thread pool executor** runs coroutines across multiple worker threads,
enabling true parallelism for CPU-bound work.

```
                 ┌─────────────────────────────────┐
                 │       ThreadPoolExecutor        │
                 ├─────────────────────────────────┤
                 │                                 │
                 │   ┌─────────────────────────┐   │
                 │   │     Work Queue          │◄──┼── Schedule(handle)
                 │   │  [Task1][Task2][Task3]  │   │
                 │   └───────────┬─────────────┘   │
                 │               │                 │
                 │     ┌────────┼────────┐        │
                 │     ▼        ▼        ▼        │
                 │  Worker1  Worker2  Worker3     │
                 │  (Thread) (Thread) (Thread)    │
                 │                                 │
                 └─────────────────────────────────┘
```

### 1.2 How It Works

The thread pool consists of:

1. **Work Queue**: A thread-safe queue of tasks to execute
2. **Worker Threads**: Multiple threads that pull work from the queue
3. **Timer Thread**: Handles delayed scheduling

```cpp
class ThreadPoolExecutor : public Executor {
public:
    explicit ThreadPoolExecutor(size_t num_threads);
    
    void Schedule(std::coroutine_handle<> handle) override;
    void ScheduleAfter(std::chrono::milliseconds delay,
                       std::coroutine_handle<> handle) override;
    void Post(std::function<void()> callback) override;
    void Run() override;
    void Stop() override;
    
private:
    std::vector<std::thread> workers_;
    std::queue<WorkItem> work_queue_;
    std::mutex queue_mutex_;
    std::condition_variable work_available_;
};
```

### 1.3 Worker Thread Loop

Each worker thread runs a loop that:
1. Waits for work to be available
2. Pops a task from the queue
3. Executes the task
4. Repeats

```cpp
void ThreadPoolExecutor::WorkerLoop() {
    while (!stopping_) {
        WorkItem item;
        
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            work_available_.wait(lock, [this] {
                return stopping_ || !work_queue_.empty();
            });
            
            if (!work_queue_.empty()) {
                item = std::move(work_queue_.front());
                work_queue_.pop();
            }
        }
        
        // Execute outside the lock
        if (item.handle) {
            item.handle.resume();
        }
    }
}
```

### 1.4 Why This Design?

**Thread Pool vs. Thread-per-Task**:
- Thread pools reuse threads (efficient)
- Thread-per-task creates overhead (expensive)

**Mutex-based Queue vs. Lock-Free**:
- Mutex is simpler and correct
- Lock-free is faster but complex
- For coroutines (fast tasks), mutex is usually fine

---

## Part 2: WhenAll - Parallel Composition

### 2.1 What is WhenAll?

`WhenAll` composes multiple tasks and waits for ALL to complete:

```cpp
auto [a, b, c] = co_await WhenAll(TaskA(), TaskB(), TaskC());
// a, b, c contain results from all three tasks
```

This is like `Promise.all()` in JavaScript.

### 2.2 How It Works

`WhenAll` runs all tasks **concurrently** using a fork/join architecture:

1. An **atomic countdown latch** is initialized to N+1 (the "+1" prevents a race where all children complete before the parent suspends)
2. Each task is wrapped in a `WhenAllTask` whose `final_suspend` decrements the latch
3. All tasks start on the calling thread and interleave at their suspension points
4. When the latch reaches zero, the parent coroutine is resumed

```cpp
auto [a, b, c] = co_await WhenAll(TaskA(), TaskB(), TaskC());
// All three tasks run concurrently, interleaving at suspension points
```

True OS-thread parallelism occurs only if tasks internally schedule onto a thread pool.

---

## Part 3: WhenAny - Racing Tasks

### 3.1 What is WhenAny?

`WhenAny` returns as soon as ANY task completes:

```cpp
auto result = co_await WhenAny(FastTask(), SlowTask());
// result is Result<WhenAnyResult<T>, std::error_code>
// result.Value().index = which task won (0 or 1)
// result.Value().value = the winner's result
```

This is like `Promise.race()` in JavaScript.

### 3.2 How It Works

`WhenAny` uses a three-layer concurrent architecture:

1. **WhenAny function**: owns an `AsyncEvent` + `optional<result>` on its frame
2. **Controller (DetachedTask)**: owns all child tasks + `atomic<bool>`; waits for ALL children via WhenAll, then self-destructs
3. **Child tasks**: each wraps one user task; race via atomic CAS

```cpp
// Each child uses compare_exchange_strong on a shared
// atomic<bool> to determine the winner. Only the winner
// writes the result and sets the event. Losers silently complete.
```

---

## Part 4: Usage Examples

### 4.1 Thread Pool Basics

```cpp
#include <hotcoco/hotcoco.hpp>
using namespace hotcoco;

int main() {
    // Create a pool with 4 worker threads
    ThreadPoolExecutor executor(4);
    
    // Post work
    executor.Post([] {
        std::cout << "Hello from thread pool!\n";
    });
    
    executor.Post([&executor] {
        std::cout << "Stopping...\n";
        executor.Stop();
    });
    
    // Run until stopped
    executor.Run();
}
```

### 4.2 Parallel Computation

```cpp
Task<int> ComputeSum(const std::vector<int>& data) {
    int sum = 0;
    for (int x : data) sum += x;
    co_return sum;
}

Task<void> ParallelSums() {
    std::vector<int> data1 = {1, 2, 3};
    std::vector<int> data2 = {4, 5, 6};
    
    auto [sum1, sum2] = co_await WhenAll(
        ComputeSum(data1),
        ComputeSum(data2)
    );
    
    std::cout << "Sum1: " << sum1 << ", Sum2: " << sum2 << "\n";
}
```

### 4.3 With Then() Chaining

Combine `Then` with `WhenAll` for powerful composition:

```cpp
Task<void> ProcessData() {
    auto pipeline1 = Then(FetchData("url1"), ParseJSON);
    auto pipeline2 = Then(FetchData("url2"), ParseJSON);
    
    auto [data1, data2] = co_await WhenAll(
        std::move(pipeline1),
        std::move(pipeline2)
    );
    
    // Both URLs fetched and parsed
    MergeData(data1, data2);
}
```

---

## Part 5: Design Deep Dive

### 5.1 Single-Threaded vs. Multi-Threaded Executors

| Feature | LibuvExecutor | ThreadPoolExecutor |
|---------|---------------|-------------------|
| Threads | 1 | N |
| I/O | Async (epoll) | Blocking |
| Best for | I/O-bound | CPU-bound |
| Coroutine safety | Implicit | Need care |

### 5.2 Executor Interface

Both executors implement the same interface:

```cpp
class Executor {
public:
    virtual void Schedule(std::coroutine_handle<>) = 0;
    virtual void ScheduleAfter(milliseconds, handle) = 0;
    virtual void Post(std::function<void()>) = 0;
    virtual void Run() = 0;
    virtual void RunOnce() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
};
```

This allows code to work with any executor:

```cpp
void ProcessTasks(Executor& executor) {
    executor.Post([&] { /* work */ });
    executor.Run();
}
```

### 5.3 Thread-Local Executor

Each thread can have a "current" executor:

```cpp
Executor* GetCurrentExecutor();
void SetCurrentExecutor(Executor*);

class ExecutorGuard {
    Executor* previous_;
public:
    ExecutorGuard(Executor* e) : previous_(GetCurrentExecutor()) {
        SetCurrentExecutor(e);
    }
    ~ExecutorGuard() { SetCurrentExecutor(previous_); }
};
```

Worker threads automatically set themselves as the current executor.

---

## Summary

| Component | Purpose |
|-----------|---------|
| `ThreadPoolExecutor` | Multi-threaded task execution |
| `WhenAll` | Wait for all tasks |
| `WhenAny` | Wait for any task |
| `Then` | Chain transformations |

### Key Takeaways

1. **Thread pools** reuse threads for efficiency
2. **WhenAll** runs all tasks concurrently with an atomic countdown latch
3. **WhenAny** races tasks with atomic CAS, returning the first result
4. **Spawn()** enables fire-and-forget detached coroutines

### Next Steps

- Explore `WithTimeout()` for deadline-based cancellation
- Use `CancellationToken` for cooperative cancellation
- Consider work-stealing scheduler for better load balancing
