# Tutorial 7: Error Handling with Result

This tutorial covers structured error handling using the `Result<T, E>` type,
the primary error handling mechanism in hotcoco (built with `-fno-exceptions`).

## What You'll Learn

1. **WHAT**: The Result type for explicit error handling
2. **HOW**: Create, inspect, and compose Results
3. **WHY**: Error handling design for a `-fno-exceptions` coroutine library

---

## Part 1: What is Result?

### 1.1 Why Result?

hotcoco is built with `-fno-exceptions`, so exceptions are not available.
`Result<T, E>` is the **only** error handling mechanism:

| Benefit | Description |
|---------|-------------|
| Explicit control flow | Errors visible in function signatures |
| Zero overhead on success | No stack unwinding |
| Composability | Chain operations with `AndThen`, `Map`, etc. |

### 1.2 Result: Explicit Error Handling

`Result<T, E>` is a sum type that holds either:
- **Ok(value)**: Success with a value of type T
- **Err(error)**: Failure with an error of type E

```cpp
Result<int, std::string> Divide(int a, int b) {
    if (b == 0) return Err("division by zero");
    return Ok(a / b);
}

auto result = Divide(10, 2);
if (result.IsOk()) {
    std::cout << result.Value() << std::endl;  // 5
} else {
    std::cerr << result.Error() << std::endl;
}
```

### 1.3 When to Use Result

| Use Result | Use std::abort / assert |
|------------|------------------------|
| Expected errors (file not found, network timeout) | Programmer errors (invariant violations) |
| Performance-critical paths | Unrecoverable internal bugs |
| Composing multiple fallible operations | Logic errors that should never happen |

---

## Part 2: Creating Results

### 2.1 Ok and Err Factory Functions

```cpp
#include "hotcoco/core/result.hpp"
using namespace hotcoco;

// Success
Result<int, std::string> ok = Ok(42);

// Error
Result<int, std::string> err = Err(std::string("something went wrong"));

// Void success (no value)
Result<void, std::string> void_ok = Ok();
```

### 2.2 How It Works Internally

```cpp
template <typename T>
struct OkTag {
    T value;
};

template <typename E>
struct ErrTag {
    E error;
};

template <typename T, typename E>
class Result {
    std::variant<T, E> data_;  // Index 0 = Ok, Index 1 = Err
};
```

### 2.3 Why This Design?

**Type Deduction**: `Ok(42)` returns `OkTag<int>`, which converts to any `Result<int, E>`.

**Zero Overhead**: On success, only the value is stored (no allocation).

---

## Part 3: Inspecting Results

### 3.1 Check and Access

```cpp
Result<int, std::string> result = ComputeValue();

// Check state
if (result.IsOk()) {
    int value = result.Value();  // Safe
}

// Boolean conversion
if (result) {
    // Ok path
}

// Safe access with default
int value = result.ValueOr(0);  // Returns 0 if error

// Safe access with optional
std::optional<int> opt = result.ValueOr();  // nullopt if error
```

### 3.2 Why Not Throw on Error Access?

```cpp
// BAD: Accessing Value() when IsErr() is undefined behavior
auto value = result.Value();  // Crash if error

// GOOD: Always check first
if (result.IsOk()) {
    auto value = result.Value();
}
```

We don't throw because hotcoco is built with `-fno-exceptions`:
1. Exceptions are not available — `Result` is the only error path
2. Allows unchecked access in performance-critical code after validation

---

## Part 4: Combinators

### 4.1 Map: Transform Success

```cpp
Result<int, std::string> result = Ok(21);

// Transform: int -> int
auto doubled = result.Map([](int x) { return x * 2; });
// doubled = Ok(42)

// Transform: int -> string
auto str = result.Map([](int x) { return std::to_string(x); });
// str = Ok("21")

// Error passes through
Result<int, std::string> err = Err("error");
auto mapped = err.Map([](int x) { return x * 2; });
// mapped = Err("error")
```

### 4.2 MapErr: Transform Error

```cpp
Result<int, int> result = Err(1);

auto mapped = result.MapErr([](int e) { 
    return std::string("Error code: ") + std::to_string(e);
});
// mapped = Err("Error code: 1")
```

### 4.3 AndThen: Chain Fallible Operations

```cpp
// Parse string to int (may fail)
Result<int, std::string> Parse(const std::string& s);

// Divide (may fail)
Result<int, std::string> Divide(int a, int b);

// Chain: parse, then divide
Result<std::string, std::string> input = Ok(std::string("10"));

auto result = std::move(input)
    .AndThen([](const std::string& s) { return Parse(s); })
    .AndThen([](int n) { return Divide(100, n); });
// result = Ok(10) if all succeed
// result = Err(...) if any fails
```

### 4.4 OrElse: Recover from Error

```cpp
Result<int, std::string> result = Err(std::string("network error"));

auto recovered = std::move(result).OrElse([](std::string e) {
    // Try fallback
    return Ok(0);  // Default value
});
// recovered = Ok(0)
```

---

## Part 5: Result with Coroutines

### 5.1 Returning Result from Tasks

```cpp
Task<Result<Data, Error>> FetchData(std::string url) {
    auto response = co_await HttpGet(url);
    if (response.status != 200) {
        co_return Err(Error{response.status, "HTTP error"});
    }
    co_return Ok(ParseData(response.body));
}
```

### 5.2 Handling Errors in Coroutines

```cpp
Task<void> ProcessData() {
    auto result = co_await FetchData("http://api.example.com");
    
    if (result.IsErr()) {
        Log("Error: ", result.Error());
        co_return;
    }
    
    UseData(result.Value());
}
```

### 5.3 Chaining with AndThen

```cpp
Task<Result<Report, Error>> GenerateReport() {
    auto data = co_await FetchData("http://api.example.com");
    if (data.IsErr()) co_return Err(data.Error());
    
    auto processed = co_await ProcessData(data.Value());
    if (processed.IsErr()) co_return Err(processed.Error());
    
    co_return Ok(CreateReport(processed.Value()));
}
```

---

## Part 6: Error Types

### 6.1 Simple Error Type

```cpp
enum class ErrorCode {
    NotFound,
    Timeout,
    InvalidInput,
};

Task<Result<Data, ErrorCode>> LoadData(std::string path);
```

### 6.2 Rich Error Type

```cpp
struct Error {
    int code;
    std::string message;
    std::string file;
    int line;
    
    static Error Make(int code, std::string msg, 
                      std::source_location loc = std::source_location::current()) {
        return {code, msg, loc.file_name(), loc.line()};
    }
};

co_return Err(Error::Make(404, "User not found"));
```

### 6.3 std::error_code Integration

```cpp
#include <system_error>

Task<Result<void, std::error_code>> WriteFile(std::string path, std::string data) {
    // ... on failure:
    co_return Err(std::make_error_code(std::errc::no_such_file_or_directory));
}
```

---

## Summary

| Feature | Purpose |
|---------|---------|
| `Ok(value)` | Create success result |
| `Err(error)` | Create error result |
| `IsOk() / IsErr()` | Check result state |
| `Value() / Error()` | Access contents |
| `ValueOr(default)` | Safe access with fallback |
| `Map()` | Transform success value |
| `MapErr()` | Transform error value |
| `AndThen()` | Chain fallible operations |
| `OrElse()` | Recover from error |

### Key Takeaways

1. **Result makes errors explicit** in function signatures
2. **Combinators enable composition** without nested if-else
3. **Works seamlessly with coroutines** via `co_return`
4. **The only error path** in hotcoco (`-fno-exceptions`); use `std::abort` for bugs
