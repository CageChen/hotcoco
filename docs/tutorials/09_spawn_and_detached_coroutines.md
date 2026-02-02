# Tutorial 9: Spawn and Detached Coroutines

This tutorial covers fire-and-forget coroutine execution using `Spawn()`,
enabling true parallel execution without awaiting.

## What You'll Learn

1. **WHAT**: Spawned coroutines and their use cases
2. **HOW**: Use Spawn() for background work
3. **WHY**: When to spawn vs await

---

## Part 1: What is Spawn?

### 1.1 The Problem

Sometimes you want to start a coroutine without waiting for it:

```cpp
Task<void> HandleRequest(Request req) {
    // Log in background - don't wait
    LogRequest(req);  // But this needs to be awaited!
    
    co_return ProcessRequest(req);
}
```

### 1.2 Spawn: Fire and Forget

`Spawn()` runs a coroutine independently:

```cpp
Task<void> HandleRequest(Request req) {
    // Fire and forget - doesn't block
    Spawn(executor, LogRequest(req));
    
    // Continue immediately
    co_return ProcessRequest(req);
}
```

### 1.3 Spawn vs Await

| Spawn | Await |
|-------|-------|
| Non-blocking | Blocking |
| No result access | Gets result |
| Runs in parallel | Runs sequentially |
| Good for side effects | Good for dependencies |

---

## Part 2: Basic Usage

### 2.1 Simple Spawn

```cpp
#include "hotcoco/core/spawn.hpp"

ThreadPoolExecutor executor(4);

// Fire and forget
Spawn(executor, BackgroundWork());

// Multiple parallel tasks
Spawn(executor, Task1());
Spawn(executor, Task2());
Spawn(executor, Task3());
```

### 2.2 With Completion Callback

```cpp
Spawn(executor, FetchData())
    .OnComplete([](Data result) {
        std::cout << "Got data: " << result << std::endl;
    });
```

### 2.3 Error Handling

```cpp
Spawn(executor, RiskyOperation())
    .OnComplete([](Result r) {
        std::cout << "Success: " << r << std::endl;
    })
    .OnError([](std::error_code ec) {
        std::cerr << "Error: " << ec.message() << std::endl;
    });
```

---

## Part 3: How It Works

### 3.1 SpawnedTask Wrapper

```cpp
template <typename T>
SpawnedTask<T> WrapForSpawn(Task<T> task) {
    co_return co_await std::move(task);
}
```

This wrapper:
1. Takes ownership of the original task
2. Manages its own lifetime (self-destructs on completion)
3. Reports results via shared state

### 3.2 Lifetime Management

```
┌─────────────────┐
│ Spawn(task)     │
└────────┬────────┘
         │ Creates
         ▼
┌─────────────────┐     ┌─────────────────┐
│ SpawnedTask     │────▶│ SharedState     │
│ (self-destruct) │     │ (result/error)  │
└─────────────────┘     └────────┬────────┘
                                 │
                                 ▼
                        ┌─────────────────┐
                        │ SpawnHandle     │
                        │ (callbacks)     │
                        └─────────────────┘
```

### 3.3 Self-Destruction

```cpp
auto final_suspend() noexcept {
    struct FinalAwaiter {
        void await_suspend(handle h) noexcept {
            h.destroy();  // Clean up after completion
        }
    };
    return FinalAwaiter{};
}
```

---

## Part 4: Use Cases

### 4.1 Background Logging

```cpp
Task<Response> HandleRequest(Request req) {
    // Log asynchronously
    Spawn(executor, LogRequest(req));
    
    auto result = co_await ProcessRequest(req);
    
    // Log result asynchronously
    Spawn(executor, LogResponse(result));
    
    co_return result;
}
```

### 4.2 Cached Refresh

```cpp
Task<Data> GetData(std::string key) {
    auto cached = cache.Get(key);
    if (cached) {
        // Refresh in background
        Spawn(executor, RefreshCache(key));
        co_return *cached;
    }
    
    // Cache miss - must wait
    auto data = co_await FetchData(key);
    cache.Set(key, data);
    co_return data;
}
```

### 4.3 Parallel Fan-Out

```cpp
Task<void> NotifyAll(std::vector<User> users, Message msg) {
    for (auto& user : users) {
        // Spawn notifications in parallel
        Spawn(executor, SendNotification(user, msg));
    }
    co_return;  // Return immediately
}
```

### 4.4 Event Processing

```cpp
Task<void> ProcessEvents(EventQueue& queue) {
    while (auto event = co_await queue.Pop()) {
        // Process each event in parallel
        Spawn(executor, HandleEvent(*event));
    }
}
```

---

## Part 5: Best Practices

### 5.1 Always Handle Errors

```cpp
// BAD: Errors silently ignored
Spawn(executor, MightFail());

// GOOD: Log errors at minimum
Spawn(executor, MightFail())
    .OnError([](std::error_code ec) {
        LogError(ec);
    });
```

### 5.2 Lifetime Awareness

```cpp
// BAD: Reference may dangle
void Process() {
    std::string data = "hello";
    Spawn(executor, UseData(data));  // data destroyed!
}

// GOOD: Capture by value or shared_ptr
void Process() {
    auto data = std::make_shared<std::string>("hello");
    Spawn(executor, UseData(*data))
        .OnComplete([data](auto) {
            // data kept alive by capture
        });
}
```

### 5.3 Limit Concurrency

```cpp
// BAD: Unbounded spawning
for (int i = 0; i < 1000000; i++) {
    Spawn(executor, Work(i));  // Too many!
}

// GOOD: Use bounded concurrency
AsyncSemaphore sem(100);
for (int i = 0; i < 1000000; i++) {
    co_await sem.Acquire();
    Spawn(executor, Work(i))
        .OnComplete([&sem](auto) { sem.Release(); });
}
```

---

## Summary

| Function | Purpose |
|----------|---------|
| `Spawn(executor, task)` | Run task detached |
| `Spawn(task)` | Use current executor |
| `.OnComplete(cb)` | Callback on success |
| `.OnError(cb)` | Callback on error |

### Key Takeaways

1. **Fire and forget**: Spawn doesn't block
2. **Self-managing lifetime**: Cleans up after completion
3. **Callback-based results**: Get notified when done
4. **Error handling**: Always register OnError for robustness
