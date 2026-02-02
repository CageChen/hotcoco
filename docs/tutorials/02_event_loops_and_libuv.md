# Tutorial 2: Event Loops and Async I/O with libuv

This tutorial explains how coroutines integrate with event loops for true asynchronous I/O, covering the `Executor`, `LibuvExecutor`, and `AsyncSleep`.

## Table of Contents

1. [The Problem: Coroutines Need a Scheduler](#the-problem-coroutines-need-a-scheduler)
2. [What Is an Event Loop?](#what-is-an-event-loop)
3. [Introduction to libuv](#introduction-to-libuv)
4. [The Executor Abstraction](#the-executor-abstraction)
5. [LibuvExecutor: libuv Backend](#libuvexecutor-libuv-backend)
6. [AsyncSleep: Non-Blocking Timers](#asyncsleep-non-blocking-timers)
7. [Thread-Local Executor Access](#thread-local-executor-access)

---

## The Problem: Coroutines Need a Scheduler

In Tutorial 1, we learned that coroutines can **suspend** and **resume**. But who decides **when** to resume them?

### The Missing Piece

```cpp
Task<std::string> FetchData() {
    auto data = co_await http_get(url);  // Suspend here...
    // ... but who resumes us when data arrives?
    co_return data;
}
```

The answer is an **event loop** (also called a scheduler or executor). It:
1. Monitors I/O operations (network, files, timers)
2. Resumes coroutines when their awaited operations complete

```
┌─────────────────────────────────────────────────────────┐
│                     Event Loop                          │
├─────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐      │
│  │ Timer Queue │  │ I/O Events  │  │ Ready Queue │      │
│  │ (AsyncSleep)│  │ (TCP, File) │  │ (Scheduled) │      │
│  └─────────────┘  └─────────────┘  └─────────────┘      │
│         │               │               │               │
│         └───────────────┼───────────────┘               │
│                         ▼                               │
│              Resume Coroutines                          │
└─────────────────────────────────────────────────────────┘
```

---

## What Is an Event Loop?

An **event loop** is a programming pattern that:
1. **Waits** for events (I/O ready, timers fired, signals)
2. **Dispatches** handlers for those events
3. **Repeats** until told to stop

### The Classic Event Loop Pattern

```cpp
void RunEventLoop() {
    while (!should_stop) {
        // 1. Wait for events (blocks if nothing to do)
        Event event = wait_for_event();
        
        // 2. Handle the event
        switch (event.type) {
            case TIMER_FIRED:
                event.callback();
                break;
            case SOCKET_READABLE:
                read_and_process(event.socket);
                break;
            // ...
        }
    }
}
```

### Under the Hood: OS Primitives

Event loops use OS-specific APIs to efficiently wait for multiple events:

| OS | API | Description |
|----|-----|-------------|
| Linux | `epoll` | Scalable I/O event notification |
| macOS | `kqueue` | BSD event notification |
| Windows | `IOCP` | I/O completion ports |

**libuv** abstracts these differences, providing a cross-platform API.

---

## Introduction to libuv

**libuv** is the async I/O library that powers Node.js. It provides:

- Cross-platform event loop
- Async TCP/UDP sockets
- Async file I/O
- Timers
- Thread pool for blocking operations

### Why libuv for hotcoco?

1. **Battle-tested**: Powers millions of Node.js applications
2. **Full-featured**: Timers, TCP, UDP, filesystem, DNS
3. **Single-threaded model**: Perfect for coroutines
4. **Linux support**: Uses `epoll` (very efficient)

### Key libuv Concepts

#### The Loop (`uv_loop_t`)

The central event loop structure:

```c
uv_loop_t loop;
uv_loop_init(&loop);

// Add handles (timers, sockets, etc.)

uv_run(&loop, UV_RUN_DEFAULT);  // Run until no more work

uv_loop_close(&loop);
```

#### Handles

Handles are long-lived objects that represent I/O resources:

```c
uv_timer_t timer;     // Timer handle
uv_tcp_t socket;      // TCP socket handle
uv_async_t async;     // Cross-thread notification
uv_idle_t idle;       // Run when loop is idle
```

#### Callbacks

libuv is callback-based. When an event occurs, your callback is invoked:

```c
void on_timer(uv_timer_t* handle) {
    printf("Timer fired!\n");
}

uv_timer_init(&loop, &timer);
uv_timer_start(&timer, on_timer, 1000, 0);  // Fire once after 1 second
```

---

## The Executor Abstraction

To make hotcoco work with different event loop backends, we define an abstract **Executor** interface:

### The Interface

```cpp
class Executor {
public:
    virtual ~Executor() = default;
    
    // Event loop control
    virtual void Run() = 0;           // Run until stopped
    virtual void RunOnce() = 0;       // One iteration
    virtual void Stop() = 0;          // Signal stop
    virtual bool IsRunning() const = 0;
    
    // Coroutine scheduling
    virtual void Schedule(std::coroutine_handle<> handle) = 0;
    virtual void ScheduleAfter(std::chrono::milliseconds delay,
                               std::coroutine_handle<> handle) = 0;
    
    // Callback support
    virtual void Post(std::function<void()> callback) = 0;
};
```

### Why Abstract?

Different backends may be better for different use cases:

| Backend | Best For |
|---------|----------|
| libuv | General purpose, networking |
| io_uring | High-performance Linux I/O |
| Custom | Testing, embedded systems |

By coding to the `Executor` interface, your coroutines work with any backend.

---

## LibuvExecutor: libuv Backend

`LibuvExecutor` implements `Executor` using libuv.

### Architecture

```
┌─────────────────────────────────────────────────────────┐
│                   LibuvExecutor                         │
├─────────────────────────────────────────────────────────┤
│  uv_loop_t loop_         ← The libuv event loop         │
│  uv_async_t async_       ← Wake up from other threads   │
│  uv_idle_t idle_         ← Process ready coroutines     │
├─────────────────────────────────────────────────────────┤
│  std::queue<coroutine_handle<>> ready_queue_            │
│  std::mutex queue_mutex_ ← Thread-safe scheduling       │
└─────────────────────────────────────────────────────────┘
```

### How Schedule() Works

When you call `executor.Schedule(handle)`:

```cpp
void LibuvExecutor::Schedule(std::coroutine_handle<> handle) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        ready_queue_.push(handle);
    }
    
    // Wake up the event loop (thread-safe)
    uv_async_send(&async_);
}
```

The `uv_async_send` is the key - it's **thread-safe** and wakes up the event loop even if it's blocked waiting for events.

### Processing the Ready Queue

The idle handle processes coroutines when the loop has nothing else to do:

```cpp
void LibuvExecutor::OnIdle(uv_idle_t* handle) {
    auto* self = static_cast<LibuvExecutor*>(handle->data);
    
    // Swap to avoid holding lock while resuming
    std::queue<std::coroutine_handle<>> ready;
    {
        std::lock_guard<std::mutex> lock(self->queue_mutex_);
        std::swap(ready, self->ready_queue_);
    }
    
    // Resume all ready coroutines
    while (!ready.empty()) {
        auto coro = ready.front();
        ready.pop();
        
        if (coro && !coro.done()) {
            coro.resume();
        }
    }
}
```

### How ScheduleAfter() Works (Timers)

For delayed scheduling, we create a libuv timer:

```cpp
void LibuvExecutor::ScheduleAfter(std::chrono::milliseconds delay,
                                   std::coroutine_handle<> handle) {
    // Create a new timer
    auto* timer = new uv_timer_t;
    uv_timer_init(&loop_, timer);
    
    // Store the coroutine handle in timer's data
    timer->data = new ScheduledHandle{handle, timer};
    
    // Start timer - fire once after delay
    uv_timer_start(timer, OnTimer, delay.count(), 0);
}

void LibuvExecutor::OnTimer(uv_timer_t* timer) {
    auto* scheduled = static_cast<ScheduledHandle*>(timer->data);
    
    // Resume the coroutine
    if (scheduled->handle && !scheduled->handle.done()) {
        scheduled->handle.resume();
    }
    
    // Cleanup
    uv_close((uv_handle_t*)timer, OnClose);
}
```

---

## AsyncSleep: Non-Blocking Timers

`AsyncSleep` is an **awaitable** that suspends a coroutine for a duration.

### Usage

```cpp
Task<void> MyTask() {
    std::cout << "Starting..." << std::endl;
    co_await AsyncSleep(1000ms);  // Non-blocking sleep!
    std::cout << "1 second later!" << std::endl;
}
```

### How It Differs from std::this_thread::sleep_for

```cpp
// BLOCKING - ties up the whole thread
void blocking_sleep() {
    std::this_thread::sleep_for(1000ms);  // Thread does nothing for 1 second
}

// NON-BLOCKING - thread can do other work
Task<void> async_sleep() {
    co_await AsyncSleep(1000ms);  // Coroutine suspends, thread is free
}
```

With blocking sleep, 1000 concurrent sleeps need 1000 threads.
With `AsyncSleep`, 1000 concurrent sleeps need **one thread** and 1000 timers.

### Implementation

```cpp
class AsyncSleep {
    std::chrono::milliseconds duration_;
    
public:
    explicit AsyncSleep(std::chrono::milliseconds duration)
        : duration_(duration) {}
    
    // Never ready immediately - always need to wait
    bool await_ready() const noexcept { return false; }
    
    // Schedule resumption after delay
    void await_suspend(std::coroutine_handle<> handle) const {
        Executor* executor = GetCurrentExecutor();
        executor->ScheduleAfter(duration_, handle);
    }
    
    // Nothing to return
    void await_resume() const noexcept {}
};
```

### The Three Awaitable Methods

| Method | Purpose | AsyncSleep Behavior |
|--------|---------|---------------------|
| `await_ready()` | Skip suspension? | Always `false` - must wait |
| `await_suspend(h)` | Schedule resumption | Call `ScheduleAfter(delay, h)` |
| `await_resume()` | Get result | Nothing to return (`void`) |

---

## Thread-Local Executor Access

How does `AsyncSleep` find the executor? Through **thread-local storage**:

```cpp
// Thread-local pointer to current executor
static thread_local Executor* g_current_executor = nullptr;

Executor* GetCurrentExecutor() {
    return g_current_executor;
}

void SetCurrentExecutor(Executor* executor) {
    g_current_executor = executor;
}
```

### ExecutorGuard: RAII Pattern

When `Run()` is called, we set the current executor and restore it on exit:

```cpp
class ExecutorGuard {
    Executor* previous_;
    
public:
    explicit ExecutorGuard(Executor* executor)
        : previous_(g_current_executor) {
        g_current_executor = executor;
    }
    
    ~ExecutorGuard() {
        g_current_executor = previous_;
    }
};

void LibuvExecutor::Run() {
    ExecutorGuard guard(this);  // Set current executor
    uv_run(&loop_, UV_RUN_DEFAULT);
}  // Restored when guard destructs
```

### Why Thread-Local?

1. **No parameter passing**: Awaitables find the executor automatically
2. **Thread safety**: Each thread has its own executor
3. **Nested executors**: The guard pattern supports nesting

---

## Putting It All Together

Here's the complete flow when you run async code:

```cpp
int main() {
    auto executor_result = LibuvExecutor::Create();
    auto& executor = *executor_result.Value();

    auto task = []() -> Task<void> {
        std::cout << "Start" << std::endl;
        co_await AsyncSleep(100ms);
        std::cout << "After 100ms" << std::endl;
    };

    auto t = task();
    executor.Schedule(t.GetHandle());
    executor.Run();
}
```

**Execution flow:**

```
1. executor.Schedule(handle)
   └─► Push handle to ready_queue_
   └─► uv_async_send() wakes the loop

2. executor.Run()
   └─► ExecutorGuard sets current executor
   └─► uv_run() starts the event loop

3. OnIdle callback fires
   └─► Pop handle from ready_queue_
   └─► handle.resume() runs coroutine

4. Coroutine runs until co_await AsyncSleep(100ms)
   └─► await_suspend() called
   └─► GetCurrentExecutor()->ScheduleAfter(100ms, handle)
   └─► New uv_timer created
   └─► Coroutine suspends

5. Event loop continues
   └─► No more ready coroutines
   └─► uv_run() waits for events

6. After 100ms, timer fires
   └─► OnTimer callback
   └─► handle.resume() continues coroutine

7. Coroutine completes
   └─► No more timers or handles
   └─► uv_run() returns

8. executor.Run() returns
   └─► ExecutorGuard restores previous executor
```

---

## Summary

| Concept | Purpose |
|---------|---------|
| Event Loop | Wait for events, dispatch handlers |
| libuv | Cross-platform async I/O library |
| Executor | Abstract event loop interface |
| LibuvExecutor | libuv-based Executor implementation |
| AsyncSleep | Non-blocking timer awaitable |
| Thread-Local Executor | Automatic executor discovery |

### Key Takeaways

1. **Coroutines need an executor** to manage scheduling and resumption
2. **libuv provides the event loop** with timers, I/O, and cross-thread signaling
3. **The Executor interface abstracts the backend** for flexibility
4. **AsyncSleep is non-blocking** - uses timers instead of blocking threads
5. **Thread-local storage** makes the executor available to awaitables

---

## Next Steps

In the next tutorial, we'll use the event loop for real I/O:
- TCP listener for accepting connections
- Async read/write on TCP streams
- Building toward an HTTP server
