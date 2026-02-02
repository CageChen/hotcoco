# Tutorial 8: Cancellation and Timeouts

This tutorial covers cooperative cancellation for coroutines using
`CancellationToken` and `CancellationSource`.

## What You'll Learn

1. **WHAT**: Cancellation tokens and cooperative cancellation
2. **HOW**: Pass tokens through coroutine chains
3. **WHY**: Graceful cleanup vs forced termination

---

## Part 1: What is Cooperative Cancellation?

### 1.1 The Problem

Long-running coroutines need a way to stop gracefully:

```cpp
Task<void> DownloadLargeFile(std::string url) {
    while (has_more_data) {
        co_await FetchChunk();  // What if user clicks "Cancel"?
    }
}
```

### 1.2 Preemptive vs Cooperative

| Type | How | Trade-offs |
|------|-----|-----------|
| **Preemptive** | Force-stop the coroutine | Resources leak, no cleanup |
| **Cooperative** | Coroutine checks if cancelled | Clean shutdown, requires polling |

### 1.3 Cancellation Token Pattern

```
┌──────────────────┐      GetToken()      ┌───────────────────┐
│ CancellationSource│─────────────────────▶│ CancellationToken │
│                  │                      │                   │
│  Cancel() ───────│────────────────────▶ │  IsCancelled()    │
└──────────────────┘                      └───────────────────┘
        │                                         │
        │                                         ▼
        │                                  ┌─────────────────┐
        └─────────────────────────────────▶│ Shared State    │
                                           │ (atomic bool)   │
                                           └─────────────────┘
```

---

## Part 2: Basic Usage

### 2.1 Creating a Token

```cpp
#include "hotcoco/core/cancellation.hpp"

CancellationSource source;
CancellationToken token = source.GetToken();

// Pass token to coroutine
auto task = LongRunningWork(token);

// Later: request cancellation
source.Cancel();
```

### 2.2 Checking Cancellation

```cpp
Task<void> Worker(CancellationToken token) {
    while (!token.IsCancelled()) {
        co_await ProcessItem();
    }
    // Clean up before returning
    co_return;
}
```

### 2.3 Multiple Tokens from One Source

```cpp
CancellationSource source;

// Multiple coroutines share the same cancellation signal
auto task1 = Worker1(source.GetToken());
auto task2 = Worker2(source.GetToken());
auto task3 = Worker3(source.GetToken());

// Cancel all at once
source.Cancel();
```

---

## Part 3: Cancellation Callbacks

### 3.1 Register Callback

```cpp
CancellationToken token = source.GetToken();

size_t handle = token.OnCancel([&] {
    std::cout << "Cancellation requested!" << std::endl;
    // Wake up waiting coroutine
    wakeup_cv.notify_all();
});
```

### 3.2 Unregister Callback

```cpp
// Manually unregister
token.Unregister(handle);

// Or use RAII guard
{
    CancellationCallbackGuard guard(token, [] {
        std::cout << "Cancelled!" << std::endl;
    });
    
    // ... do work ...
    
}  // Callback automatically unregistered
```

### 3.3 Why Callbacks?

Callbacks enable waking up blocked operations:

```cpp
Task<void> WaitWithCancellation(CancellationToken token) {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    
    // Register callback to wake up wait
    CancellationCallbackGuard guard(token, [&] {
        std::lock_guard<std::mutex> lock(mutex);
        done = true;
        cv.notify_all();
    });
    
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return done || token.IsCancelled(); });
    
    co_return;
}
```

---

## Part 4: Integration with Coroutines

### 4.1 Passing Through Chains

```cpp
Task<Data> ProcessPipeline(CancellationToken token) {
    auto raw = co_await FetchData(token);
    if (token.IsCancelled()) co_return {};
    
    auto cleaned = co_await CleanData(raw, token);
    if (token.IsCancelled()) co_return {};
    
    auto result = co_await TransformData(cleaned, token);
    co_return result;
}
```

### 4.2 Early Exit Pattern

```cpp
Task<Result<Data, Error>> FetchWithCancel(
    std::string url, CancellationToken token) 
{
    if (token.IsCancelled()) {
        co_return Err(Error::Cancelled);
    }
    
    auto response = co_await HttpGet(url);
    
    if (token.IsCancelled()) {
        co_return Err(Error::Cancelled);
    }
    
    co_return Ok(ParseResponse(response));
}
```

### 4.3 WithTimeout (Built-in)

hotcoco provides `WithTimeout()` for deadline-based cancellation:

```cpp
#include "hotcoco/core/timeout.hpp"

Task<void> FetchWithDeadline() {
    auto result = co_await WithTimeout(FetchData(), 5s);
    if (result.IsErr()) {
        // result.Error() is TimeoutError
        std::cerr << result.Error().Message() << std::endl;
        co_return;
    }
    UseData(result.Value());
}
```

`WithTimeout()` internally races two tasks (user task vs timer) via atomic CAS.
It returns `Result<T, TimeoutError>` — `Ok(value)` if the task completes in time,
`Err(TimeoutError)` if the deadline expires.

---

## Part 5: Thread Safety

### 5.1 Cancel from Any Thread

```cpp
CancellationSource source;

// Main thread starts work
auto task = ProcessData(source.GetToken());

// Background thread monitors for shutdown signal
std::thread monitor([&source] {
    WaitForShutdownSignal();
    source.Cancel();  // Thread-safe!
});
```

### 5.2 How It Works

```cpp
class CancellationState {
    std::atomic<bool> cancelled_{false};
    
    void Cancel() {
        // Atomic exchange - only first Cancel() returns false
        if (cancelled_.exchange(true)) {
            return;  // Already cancelled
        }
        // Invoke callbacks...
    }
    
    bool IsCancelled() const {
        return cancelled_.load(std::memory_order_acquire);
    }
};
```

---

## Part 6: Best Practices

### 6.1 Check at Suspension Points

```cpp
Task<void> Process(CancellationToken token) {
    for (auto& item : items) {
        // Check BEFORE expensive operation
        if (token.IsCancelled()) break;
        
        co_await ProcessItem(item);
        
        // Check AFTER to avoid wasted work
        if (token.IsCancelled()) break;
    }
}
```

### 6.2 Resource Cleanup

```cpp
Task<void> WithCleanup(CancellationToken token) {
    auto resource = AcquireResource();
    
    while (!token.IsCancelled()) {
        co_await DoWork();
    }
    
    // Always clean up, even on cancellation
    resource.Release();
}
```

### 6.3 Don't Ignore Cancellation

```cpp
// BAD: Ignores cancellation
Task<void> BadWorker(CancellationToken token) {
    while (true) {
        co_await DoWork();
    }
}

// GOOD: Respects cancellation
Task<void> GoodWorker(CancellationToken token) {
    while (!token.IsCancelled()) {
        co_await DoWork();
    }
}
```

---

## Summary

| Component | Purpose |
|-----------|---------|
| `CancellationSource` | Creates tokens, triggers cancellation |
| `CancellationToken` | Read-only view, passed to coroutines |
| `IsCancelled()` | Check if cancellation requested |
| `OnCancel()` | Register callback for notification |
| `CancellationCallbackGuard` | RAII callback management |

### Key Takeaways

1. **Cooperative**: Coroutines must check the token
2. **Thread-safe**: Cancel from any thread
3. **Callbacks**: Wake up blocked operations
4. **Clean shutdown**: Opportunity for resource cleanup
