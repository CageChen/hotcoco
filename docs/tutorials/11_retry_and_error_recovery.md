# Tutorial 11: Retry and Error Recovery

This tutorial covers automatic retry logic for transient failures
with configurable backoff strategies.

## What You'll Learn

1. **WHAT**: The Retry utility for fault tolerance
2. **HOW**: Configure retry policies with exponential backoff
3. **WHY**: Build resilient systems that handle transient failures

---

## Part 1: Why Retry?

### 1.1 Transient Failures

Many failures are temporary:
- Network timeouts
- Database connection limits
- Rate limiting (429 errors)
- Service restarts

### 1.2 Without Retry

```cpp
Task<Result<Data, std::error_code>> FetchData() {
    auto result = co_await HttpGet(url);
    if (result.IsErr()) {
        // Just fail - not resilient
        co_return Err(result.Error());
    }
    co_return Ok(result.Value());
}
```

### 1.3 With Retry

```cpp
Task<Result<Data, std::error_code>> FetchData() {
    co_return co_await Retry([]() {
        return HttpGet(url);
    }).MaxAttempts(3)
      .WithExponentialBackoff(100ms)
      .Execute();
}
```

---

## Part 2: Basic Usage

### 2.1 Simple Retry

```cpp
#include "hotcoco/core/retry.hpp"

Task<Result<Response, std::error_code>> ResilientCall() {
    co_return co_await Retry([]() {
        return CallAPI();
    }).Execute();  // Default: 3 attempts, 100ms initial delay
}
```

### 2.2 Configuring Attempts

```cpp
Task<Result<Response, std::error_code>> Call() {
    co_return co_await Retry([]() {
        return CallAPI();
    }).MaxAttempts(5)  // Try up to 5 times
      .Execute();
}
```

### 2.3 Exponential Backoff

```cpp
Task<Result<Response, std::error_code>> Call() {
    co_return co_await Retry([]() {
        return CallAPI();
    }).WithExponentialBackoff(100ms, 2.0)
      // Delays: 100ms, 200ms, 400ms, 800ms, ...
      .MaxDelay(30s)  // Cap at 30 seconds
      .Execute();
}
```

---

## Part 3: Advanced Configuration

### 3.1 Jitter

By default, jitter is added to prevent thundering herd:

```cpp
// With jitter (default)
// Delay = base * random(0.5, 1.5)
// Example: 100ms becomes 50-150ms

// Without jitter
Retry(factory)
    .WithExponentialBackoff(100ms)
    .NoJitter()
    .Execute();
```

### 3.2 Conditional Retry

Only retry certain errors:

```cpp
Retry(factory)
    .RetryIf([](std::error_code ec) {
        // Only retry network-related errors
        return ec.category() == std::system_category()
            && (ec.value() == ECONNREFUSED || ec.value() == ETIMEDOUT);
    })
    .Execute();
```

### 3.3 Non-Retryable Errors

```cpp
// Retry everything except permission errors
Retry(factory)
    .RetryIf([](std::error_code ec) {
        return ec != std::errc::permission_denied
            && ec != std::errc::invalid_argument;
    })
    .Execute();
```

---

## Part 4: How It Works

### 4.1 Retry Loop

```
┌─────────────┐
│ Attempt 1   │ ──▶ Success? ──▶ Return Ok(result)
└─────────────┘         │
                        ▼ Fail
                ┌─────────────┐
                │ Retryable?  │ ──▶ No ──▶ Return Err(error)
                └─────────────┘
                        │ Yes
                        ▼
                ┌─────────────┐
                │ Wait delay  │
                └─────────────┘
                        │
                        ▼
┌─────────────┐
│ Attempt 2   │ ──▶ ...
└─────────────┘
```

### 4.2 Backoff Calculation

```
Delay(n) = min(initial * multiplier^n + jitter, max_delay)

Example with initial=100ms, multiplier=2.0:
  Attempt 1: fail → wait ~100ms
  Attempt 2: fail → wait ~200ms
  Attempt 3: fail → wait ~400ms
  ...
```

The task factory must return `Task<Result<T, std::error_code>>`.
`Execute()` returns `Task<Result<T, std::error_code>>` — `Ok(value)` on
success, `Err(last_error)` after all attempts are exhausted.

---

## Part 5: Patterns

### 5.1 Database Reconnection

```cpp
Task<Result<Connection, std::error_code>> GetConnection() {
    co_return co_await Retry([]() {
        return db_pool.Acquire();
    }).MaxAttempts(5)
      .WithExponentialBackoff(500ms, 2.0)
      .MaxDelay(10s)
      .RetryIf([](std::error_code ec) {
          // Only retry connection-related errors
          return ec.value() == ECONNREFUSED || ec.value() == ECONNRESET;
      })
      .Execute();
}
```

### 5.2 API Call with Rate Limiting

```cpp
Task<Result<Response, std::error_code>> CallWithRateLimit() {
    co_return co_await Retry([]() {
        return api_client.Request();
    }).MaxAttempts(10)
      .WithExponentialBackoff(1s, 2.0)
      .MaxDelay(60s)
      .RetryIf([](std::error_code ec) {
          // Retry on rate-limit and server errors
          return ec == HttpError::TooManyRequests
              || ec == HttpError::InternalServerError;
      })
      .Execute();
}
```

### 5.3 File Operations

```cpp
Task<Result<void, std::error_code>> WriteWithRetry(std::string path, std::string data) {
    co_return co_await Retry([&]() {
        return fs.Write(path, data);
    }).MaxAttempts(3)
      .WithExponentialBackoff(100ms)
      .RetryIf([](std::error_code ec) {
          // Retry on temporary I/O errors, not permission errors
          return ec != std::errc::permission_denied;
      })
      .Execute();
}
```

---

## Part 6: Best Practices

### 6.1 Set Maximum Attempts

Always limit retries:
```cpp
.MaxAttempts(5)  // Don't retry forever
```

### 6.2 Use Exponential Backoff

Linear or no backoff can overwhelm failing services:
```cpp
// GOOD: Exponential backoff
.WithExponentialBackoff(100ms, 2.0)

// AVOID: No backoff (immediate retries)
```

### 6.3 Cap Maximum Delay

Prevent excessively long waits:
```cpp
.MaxDelay(30s)  // Cap delay
```

### 6.4 Be Selective

Don't retry non-transient errors:
```cpp
.RetryIf([](std::error_code ec) {
    // Don't retry auth failures or validation errors
    if (ec == AppError::AuthFailed) return false;
    if (ec == AppError::ValidationFailed) return false;
    return true;
})
```

---

## Summary

| Method | Purpose |
|--------|---------|
| `Retry(factory)` | Create retry builder |
| `.MaxAttempts(n)` | Set maximum tries |
| `.WithExponentialBackoff(initial, mult)` | Configure backoff |
| `.MaxDelay(duration)` | Cap delay time |
| `.NoJitter()` | Disable randomization |
| `.RetryIf(predicate)` | Conditional retry (`std::error_code` predicate) |
| `.Execute()` | Run with retry logic, returns `Task<Result<T, std::error_code>>` |

### Key Takeaways

1. **Transient failures happen**: Network, database, rate limits
2. **Exponential backoff**: Prevents overwhelming failing services
3. **Jitter**: Avoids thundering herd
4. **Be selective**: Don't retry permanent errors
