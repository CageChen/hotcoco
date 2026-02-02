# Tutorial 10: Synchronization Primitives

This tutorial covers coroutine-aware synchronization primitives that
allow safe coordination between concurrent tasks without blocking threads.

## What You'll Learn

1. **WHAT**: AsyncMutex, AsyncSemaphore, AsyncRWLock
2. **HOW**: Protect shared state in coroutines
3. **WHY**: Non-blocking synchronization for high concurrency

---

## Part 1: The Problem with std::mutex

### 1.1 Why Not std::mutex?

Standard mutexes block the calling thread:

```cpp
std::mutex mutex;

Task<void> BadExample() {
    std::lock_guard lock(mutex);  // BLOCKS THREAD!
    // If coroutine suspends here, thread is stuck
    co_await SomeAsyncWork();
    // Lock held across suspension - BAD!
}
```

Problems:
1. Blocks thread pool workers
2. Reduces parallelism
3. Can cause deadlocks with coroutines

### 1.2 Solution: Async-Aware Primitives

Our primitives **suspend** instead of block:

```cpp
AsyncMutex mutex;

Task<void> GoodExample() {
    auto lock = co_await mutex.Lock();  // SUSPENDS if busy
    // When we resume, lock is held
    DoWork();
}  // Automatically released
```

---

## Part 2: AsyncMutex

### 2.1 Basic Usage

```cpp
#include "hotcoco/sync/mutex.hpp"

AsyncMutex mutex;
int shared_counter = 0;

Task<void> Increment() {
    auto lock = co_await mutex.Lock();
    shared_counter++;
    // Lock released when 'lock' goes out of scope
}
```

### 2.2 TryLock for Non-Blocking Attempts

```cpp
Task<void> TryIncrement() {
    auto maybe_lock = co_await mutex.TryLock();
    if (maybe_lock) {
        shared_counter++;
    } else {
        // Mutex was busy, do something else
    }
}
```

### 2.3 How It Works

```
Coroutine A calls Lock():
┌─────────────┐
│ await_ready │ ──▶ Check if unlocked
└─────────────┘     If yes: acquire, return true
       │
       │ No
       ▼
┌─────────────┐
│await_suspend│ ──▶ Add to waiters queue
└─────────────┘     Return true (suspend)
       │
       │ Later, when unlocked
       ▼
┌─────────────┐
│await_resume │ ──▶ Return ScopedLock
└─────────────┘
```

---

## Part 3: AsyncSemaphore

### 3.1 Limiting Concurrency

Semaphores control how many tasks run simultaneously:

```cpp
#include "hotcoco/sync/semaphore.hpp"

AsyncSemaphore sem(3);  // Allow 3 concurrent operations

Task<void> RateLimitedWork() {
    auto guard = co_await sem.Acquire();
    // At most 3 tasks execute here concurrently
    co_await DoExpensiveWork();
}  // Automatically released
```

### 3.2 Use Cases

**Database connection pool:**
```cpp
AsyncSemaphore db_connections(10);

Task<QueryResult> Query(std::string sql) {
    auto guard = co_await db_connections.Acquire();
    return co_await ExecuteQuery(sql);
}
```

**Rate limiting:**
```cpp
AsyncSemaphore api_limit(100);  // 100 concurrent API calls

Task<Response> CallAPI(Request req) {
    auto guard = co_await api_limit.Acquire();
    return co_await HttpClient::Post(req);
}
```

---

## Part 4: AsyncRWLock

### 4.1 Multiple Readers, Single Writer

```cpp
#include "hotcoco/sync/rwlock.hpp"

AsyncRWLock lock;
std::map<std::string, std::string> cache;

Task<std::string> Read(std::string key) {
    auto guard = co_await lock.ReadLock();
    return cache[key];  // Multiple readers OK
}

Task<void> Write(std::string key, std::string value) {
    auto guard = co_await lock.WriteLock();
    cache[key] = value;  // Exclusive access
}
```

### 4.2 When to Use RWLock

| Scenario | Use |
|----------|-----|
| Read-heavy workload | AsyncRWLock |
| Equal read/write | AsyncMutex |
| Simple protection | AsyncMutex |

### 4.3 Fair Policy

Our RWLock uses a **fair policy** that alternates between readers and writers:
- New readers are blocked when writers are waiting (prevents writer starvation)
- On write unlock, pending readers are woken first (prevents reader starvation)

```
Time ─────────────────────────────────────────────▶
R1  ████████
R2      ████████
W1          ░░░░████████  (waits for readers, then runs)
R3                  ████  (woken first after writer finishes)
```

---

## Part 5: Best Practices

### 5.1 Keep Critical Sections Short

```cpp
// BAD: Long critical section
Task<void> Bad() {
    auto lock = co_await mutex.Lock();
    auto data = co_await FetchFromNetwork();  // Long wait!
    Process(data);
}

// GOOD: Minimize lock scope
Task<void> Good() {
    auto data = co_await FetchFromNetwork();  // Fetch first
    {
        auto lock = co_await mutex.Lock();
        Process(data);  // Only lock for processing
    }
}
```

### 5.2 Avoid Lock Hierarchies

```cpp
// DANGEROUS: Can deadlock
Task<void> Danger() {
    auto lock1 = co_await mutex1.Lock();
    auto lock2 = co_await mutex2.Lock();  // If another task does reverse order...
}

// SAFE: Consistent ordering or single lock
Task<void> Safe() {
    auto lock = co_await combined_mutex.Lock();
    // Access both resources
}
```

### 5.3 Consider Lock-Free Alternatives

For simple counters, atomics are faster:
```cpp
std::atomic<int> counter{0};

Task<void> Increment() {
    counter++;  // No lock needed
    co_return;
}
```

---

## Part 6: Comparison

| Primitive | Purpose | Waiters |
|-----------|---------|---------|
| AsyncMutex | Exclusive access | FIFO queue |
| AsyncSemaphore | Bounded concurrency | FIFO queue |
| AsyncRWLock | Read/write separation | Fair (alternating) |

---

## Summary

| Class | When to Use |
|-------|-------------|
| `AsyncMutex` | Protecting shared state |
| `AsyncSemaphore` | Limiting concurrent operations |
| `AsyncRWLock` | Read-heavy workloads with occasional writes |

### Key Takeaways

1. **Suspend, don't block**: Async primitives yield instead of blocking
2. **RAII guards**: Automatic cleanup via scope guards
3. **Fair wakeup**: FIFO ordering prevents starvation
4. **Keep it short**: Minimize time holding locks
