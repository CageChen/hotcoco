# Tutorial 1: C++20 Coroutine Fundamentals

This tutorial explains the core coroutine primitives in hotcoco: `Task<T>`, `Generator<T>`, and `SyncWait()`.

## Table of Contents

1. [What Are Coroutines?](#what-are-coroutines)
2. [The Three Coroutine Keywords](#the-three-coroutine-keywords)
3. [How Coroutines Work Internally](#how-coroutines-work-internally)
4. [Task\<T\>: The Lazy Async Primitive](#taskt-the-lazy-async-primitive)
5. [Generator\<T\>: Lazy Sequences](#generatort-lazy-sequences)
6. [SyncWait: Bridging Sync and Async](#syncwait-bridging-sync-and-async)

---

## What Are Coroutines?

A **coroutine** is a function that can **suspend its execution** and **resume later**. Unlike regular functions that run from start to finish, coroutines can pause mid-execution, save their state, and continue from where they left off.

### Regular Function vs Coroutine

```cpp
// Regular function - runs to completion
int regular_function() {
    int x = compute_something();  // Must wait here
    int y = compute_more(x);      // Can't start until x is ready
    return x + y;
}

// Coroutine - can suspend and resume
Task<int> coroutine_function() {
    int x = co_await compute_something();  // Suspend here, resume when ready
    int y = co_await compute_more(x);      // Suspend again
    co_return x + y;
}
```

### Why Use Coroutines?

1. **Non-blocking I/O**: Instead of blocking a thread waiting for I/O, suspend the coroutine
2. **Memory efficiency**: One thread can run thousands of coroutines
3. **Readable async code**: Write async code that looks like sync code

---

## The Three Coroutine Keywords

C++20 introduces three keywords that make a function a coroutine:

### `co_await` - Suspend and Wait

```cpp
Task<std::string> FetchData() {
    // co_await suspends this coroutine until the network call completes
    std::string data = co_await http_get("https://example.com");
    co_return data;
}
```

When you `co_await` something:
1. The expression is checked: "Is it ready?" (`await_ready()`)
2. If not ready, the coroutine suspends (`await_suspend()`)
3. When resumed, get the result (`await_resume()`)

### `co_return` - Return a Value and Finish

```cpp
Task<int> ComputeSum(int a, int b) {
    co_return a + b;  // Return value and complete the coroutine
}
```

For `Task<void>`, you can omit `co_return` or use `co_return;`:

```cpp
Task<void> DoWork() {
    // ... do something ...
    co_return;  // Optional for void
}
```

### `co_yield` - Produce a Value and Suspend

```cpp
Generator<int> Range(int start, int end) {
    for (int i = start; i < end; ++i) {
        co_yield i;  // Produce i, suspend, wait for next request
    }
}
```

`co_yield` is perfect for lazy sequences - each value is computed only when requested.

---

## How Coroutines Work Internally

When the compiler sees a coroutine, it transforms the function into a state machine. Here's what happens:

### The Coroutine Frame

The compiler allocates a **coroutine frame** on the heap containing:
- Local variables
- Parameters
- Current suspension point
- The **promise object**

```
┌─────────────────────────────────────────┐
│           Coroutine Frame               │
├─────────────────────────────────────────┤
│  Promise Object                         │
│  - Stores result                        │
│  - Customization points                 │
├─────────────────────────────────────────┤
│  Parameters (copied/moved)              │
├─────────────────────────────────────────┤
│  Local Variables                        │
├─────────────────────────────────────────┤
│  Suspension Point Index                 │
└─────────────────────────────────────────┘
```

### The Promise Type

Every coroutine has a **promise type** that controls its behavior. The compiler finds it via:

```cpp
// The compiler looks for:
typename Task<T>::promise_type
```

The promise type must implement these **customization points**:

```cpp
class TaskPromise {
    // Called to create the return object (Task<T>)
    Task<T> get_return_object();
    
    // Should we suspend before starting? (lazy vs eager)
    auto initial_suspend();
    
    // Should we suspend at the end? (for cleanup/chaining)
    auto final_suspend();
    
    // Handle co_return value;
    void return_value(T value);
    
    // Handle unrecoverable errors (calls std::abort under -fno-exceptions)
    void unhandled_exception();
};
```

### The Coroutine Handle

A `std::coroutine_handle<Promise>` is a pointer to the coroutine frame:

```cpp
std::coroutine_handle<TaskPromise<int>> handle;

handle.resume();   // Continue execution
handle.done();     // Is the coroutine finished?
handle.destroy();  // Free the coroutine frame
handle.promise();  // Access the promise object
```

---

## Task\<T\>: The Lazy Async Primitive

`Task<T>` is the fundamental coroutine type in hotcoco. It represents a computation that:
- **Produces one value** of type `T`
- **Starts lazily** (doesn't run until awaited)
- **Uses symmetric transfer** for efficient chaining

### Basic Usage

```cpp
Task<int> Add(int a, int b) {
    co_return a + b;
}

Task<int> Multiply(int a, int b) {
    co_return a * b;
}

Task<int> Compute() {
    int sum = co_await Add(3, 4);      // 7
    int product = co_await Multiply(sum, 2);  // 14
    co_return product;
}
```

### Why Lazy Execution?

```cpp
Task<int> LazyTask() {
    std::cout << "Starting!" << std::endl;
    co_return 42;
}

int main() {
    auto task = LazyTask();  // Nothing printed yet!
    std::cout << "Task created" << std::endl;
    
    int result = SyncWait(std::move(task));  // "Starting!" printed now
    std::cout << "Result: " << result << std::endl;
}

// Output:
// Task created
// Starting!
// Result: 42
```

Lazy execution gives you control over **when** work happens.

### How Task\<T\> Works

Here's the simplified structure:

```cpp
template <typename T>
class Task {
public:
    // The promise type the compiler uses
    class promise_type {
        std::optional<T> result_;
        std::coroutine_handle<> continuation_;  // Who to resume when done
        
        // Create the Task object
        Task get_return_object() {
            return Task{Handle::from_promise(*this)};
        }
        
        // Lazy: suspend immediately
        std::suspend_always initial_suspend() { return {}; }
        
        // Store the result
        void return_value(T value) { result_ = std::move(value); }
        
        // When done, resume whoever was waiting for us
        auto final_suspend() noexcept {
            struct Awaiter {
                std::coroutine_handle<> continuation_;
                bool await_ready() noexcept { return false; }
                std::coroutine_handle<> await_suspend(Handle) noexcept {
                    return continuation_;  // Symmetric transfer!
                }
                void await_resume() noexcept {}
            };
            return Awaiter{continuation_};
        }
    };

    // Make Task awaitable
    bool await_ready() { return false; }
    
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) {
        // Store who's waiting for us
        handle_.promise().continuation_ = caller;
        return handle_;  // Transfer control to this task
    }
    
    T await_resume() {
        return std::move(handle_.promise().result_.value());
    }
};
```

### Symmetric Transfer Explained

When coroutine A awaits coroutine B, we need to:
1. Suspend A
2. Start/resume B
3. When B finishes, resume A

**Without symmetric transfer** (stack overflow risk):
```cpp
void await_suspend(std::coroutine_handle<> caller) {
    handle_.resume();  // <-- This CALLS resume(), growing the stack
}
```

**With symmetric transfer** (safe):
```cpp
std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) {
    return handle_;  // <-- Return handle, let the runtime switch
}
```

The key insight: returning a handle from `await_suspend` tells the runtime "switch to this coroutine" without a function call, keeping the stack flat.

---

## Generator\<T\>: Lazy Sequences

`Generator<T>` produces a sequence of values lazily using `co_yield`.

### Basic Usage

```cpp
Generator<int> Range(int start, int end) {
    for (int i = start; i < end; ++i) {
        co_yield i;
    }
}

// Using range-for
for (int x : Range(0, 5)) {
    std::cout << x << " ";  // 0 1 2 3 4
}
```

### How Generator Works

```cpp
template <typename T>
class Generator {
public:
    class promise_type {
        T* current_value_;  // Pointer to the yielded value
        
        // Lazy start
        std::suspend_always initial_suspend() { return {}; }
        
        // Must suspend at end so iterator can detect completion
        std::suspend_always final_suspend() { return {}; }
        
        // Handle co_yield
        std::suspend_always yield_value(T& value) {
            current_value_ = &value;
            return {};  // Suspend after yielding
        }
        
        void return_void() {}  // Generators don't return values
    };
    
    // Iterator for range-for support
    class iterator {
        Handle handle_;
        
        iterator& operator++() {
            handle_.resume();
            if (handle_.done()) handle_ = nullptr;
            return *this;
        }
        
        T& operator*() {
            return *handle_.promise().current_value_;
        }
    };
    
    iterator begin() {
        handle_.resume();  // Get first value
        return iterator{handle_.done() ? nullptr : handle_};
    }
    
    iterator end() { return iterator{nullptr}; }
};
```

### Why Lazy Sequences Matter

```cpp
// Infinite sequence - only compute what you need
Generator<int> Fibonacci() {
    int a = 0, b = 1;
    while (true) {
        co_yield a;
        int next = a + b;
        a = b;
        b = next;
    }
}

// Take first 10 Fibonacci numbers
auto fib = Fibonacci();
auto it = fib.begin();
for (int i = 0; i < 10; ++i) {
    std::cout << *it << " ";
    ++it;
}
// Output: 0 1 1 2 3 5 8 13 21 34
```

---

## SyncWait: Bridging Sync and Async

`SyncWait()` runs a coroutine from synchronous code and blocks until completion.

### Usage

```cpp
Task<int> AsyncComputation() {
    co_return 42;
}

int main() {
    // main() is not a coroutine, can't use co_await
    int result = SyncWait(AsyncComputation());
    std::cout << result << std::endl;  // 42
}
```

### How SyncWait Works

```cpp
template <typename T>
T SyncWait(Task<T> task) {
    std::optional<T> result;
    bool done = false;
    
    // Create a wrapper coroutine that stores the result
    auto wrapper = [&]() -> Task<void> {
        result = co_await std::move(task);
        done = true;
    };
    
    auto wrapper_task = wrapper();
    auto handle = wrapper_task.GetHandle();
    
    // Run the coroutine synchronously
    handle.resume();
    
    // In a real implementation with an event loop,
    // we'd spin the loop here until done
    
    return std::move(result.value());
}
```

The key insight: `SyncWait` creates a bridge between the synchronous world (main, callbacks) and the asynchronous world (coroutines).

---

## Summary

| Concept | Purpose | Keyword |
|---------|---------|---------|
| `Task<T>` | Single async value | `co_await`, `co_return` |
| `Generator<T>` | Lazy sequence | `co_yield` |
| `SyncWait()` | Sync/async bridge | N/A |
| Promise Type | Coroutine behavior | Compiler magic |
| Coroutine Handle | Control coroutine | `std::coroutine_handle` |

### Key Takeaways

1. **Coroutines are state machines** - The compiler transforms your function
2. **The Promise Type controls behavior** - `initial_suspend`, `final_suspend`, etc.
3. **Lazy execution gives control** - Work only happens when you await
4. **Symmetric transfer prevents stack overflow** - Return handles, don't call resume
5. **Generators are single-pass iterators** - Perfect for lazy sequences

---

## Next: Tutorial 2

In the next tutorial, we'll explore how to integrate coroutines with an event loop (libuv) for true asynchronous I/O.
