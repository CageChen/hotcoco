# Tutorial 6: Debugging with Sanitizers

This tutorial covers using sanitizers to detect memory errors, data races,
and undefined behavior in coroutine-based code.

## What You'll Learn

1. **WHAT**: Sanitizers and what bugs they catch
2. **HOW**: Enable and use sanitizers with hotcoco
3. **WHY**: Coroutines have unique debugging challenges

---

## Part 1: What Are Sanitizers?

### 1.1 Overview

Sanitizers are compiler-based tools that instrument code to detect bugs at runtime:

| Sanitizer | Detects |
|-----------|---------|
| **AddressSanitizer (ASan)** | Memory errors: buffer overflows, use-after-free, leaks |
| **ThreadSanitizer (TSan)** | Data races, deadlocks |
| **UndefinedBehaviorSanitizer (UBSan)** | Undefined behavior: signed overflow, null dereference |

### 1.2 Why Sanitizers Matter for Coroutines

Coroutines have unique memory patterns:
- **Heap-allocated frames**: Coroutine state lives on the heap
- **Deferred destruction**: Frame lifetime extends beyond function return
- **Resumed from anywhere**: Same frame accessed from multiple call sites

Common coroutine bugs:
- Use-after-free when awaiting destroyed task
- Data races in shared state for WhenAll/WhenAny
- Leaks from forgotten co_await

---

## Part 2: Enabling Sanitizers

### 2.1 CMake Options

```bash
# Build with AddressSanitizer
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON

# Build with ThreadSanitizer
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON

# Build with UndefinedBehaviorSanitizer
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_UBSAN=ON

# Combine ASan + UBSan (but NOT TSan with ASan)
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
```

### 2.2 How It Works

CMake adds these flags:

```cmake
# ASan
target_compile_options(hotcoco PUBLIC -fsanitize=address -fno-omit-frame-pointer)
target_link_options(hotcoco PUBLIC -fsanitize=address)

# TSan
target_compile_options(hotcoco PUBLIC -fsanitize=thread)
target_link_options(hotcoco PUBLIC -fsanitize=thread)

# UBSan
target_compile_options(hotcoco PUBLIC -fsanitize=undefined)
target_link_options(hotcoco PUBLIC -fsanitize=undefined)
```

---

## Part 3: AddressSanitizer (ASan)

### 3.1 What ASan Detects

- **Heap buffer overflow**: Writing past allocated memory
- **Stack buffer overflow**: Overflowing stack buffers
- **Use-after-free**: Accessing freed memory
- **Use-after-return**: Stack use after function return
- **Memory leaks**: Unreleased allocations

### 3.2 Example: Detecting Use-After-Free

```cpp
Task<int> DangerousTask() {
    int* ptr = new int(42);
    delete ptr;
    co_return *ptr;  // ASan: heap-use-after-free
}
```

**ASan Output**:
```
==12345==ERROR: AddressSanitizer: heap-use-after-free
READ of size 4 at 0x602000000010
    #0 DangerousTask() task.cpp:5
    #1 main main.cpp:10
```

### 3.3 Coroutine-Specific: Awaiting Destroyed Task

```cpp
Task<int> GetValue() {
    co_return 42;
}

void Bug() {
    Task<int> task = GetValue();
    auto handle = task.GetHandle();
    // Task destructor destroys coroutine
}  // task destroyed here

void Crash() {
    handle.resume();  // ASan: heap-use-after-free
}
```

---

## Part 4: ThreadSanitizer (TSan)

### 4.1 What TSan Detects

- **Data races**: Unsynchronized concurrent access
- **Lock order violations**: Potential deadlocks
- **Thread leaks**: Unjoinable threads

### 4.2 Example: Data Race in Shared State

```cpp
int shared_counter = 0;

Task<void> Increment() {
    shared_counter++;  // TSan: data race
    co_return;
}

void Bug() {
    ThreadPoolExecutor executor(4);
    for (int i = 0; i < 100; i++) {
        executor.Post([&] { 
            auto t = Increment();
            t.GetHandle().resume();
        });
    }
}
```

**TSan Output**:
```
WARNING: ThreadSanitizer: data race
  Write of size 4 at 0x... by thread T2:
    #0 Increment() example.cpp:4
  Previous write of size 4 at 0x... by thread T1:
    #0 Increment() example.cpp:4
```

### 4.3 Fixing Data Races

```cpp
std::atomic<int> shared_counter{0};

Task<void> Increment() {
    shared_counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}
```

---

## Part 5: UndefinedBehaviorSanitizer (UBSan)

### 5.1 What UBSan Detects

- **Signed integer overflow**
- **Null pointer dereference**
- **Invalid shift amounts**
- **Out-of-bounds array access**
- **Type mismatch**

### 5.2 Example: Signed Overflow

```cpp
Task<int> Overflow() {
    int x = INT_MAX;
    co_return x + 1;  // UBSan: signed integer overflow
}
```

---

## Part 6: Best Practices

### 6.1 Development Workflow

```bash
# Regular development: fast builds
cmake -B build -DCMAKE_BUILD_TYPE=Debug
ninja -C build && ctest --test-dir build

# Before commit: run with ASan
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
ninja -C build-asan && ctest --test-dir build-asan

# For thread pool code: run with TSan
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
ninja -C build-tsan && ctest --test-dir build-tsan -R ThreadPool
```

### 6.2 Suppression Files

Create `asan.supp` for false positives:

```
# Suppress leak in third-party library
leak:some_library_function
```

Run with:
```bash
ASAN_OPTIONS=suppressions=asan.supp ./hotcoco_test
```

### 6.3 Performance Impact

| Sanitizer | Slowdown | Memory Overhead |
|-----------|----------|-----------------|
| ASan | 2x | 2-3x |
| TSan | 5-15x | 5-10x |
| UBSan | 1.2x | Minimal |

**Note**: Don't use sanitizers in production - they're for testing only.

---

## Summary

| Sanitizer | Use Case | CMake Flag |
|-----------|----------|------------|
| ASan | Memory bugs, leaks | `-DENABLE_ASAN=ON` |
| TSan | Data races, thread bugs | `-DENABLE_TSAN=ON` |
| UBSan | Undefined behavior | `-DENABLE_UBSAN=ON` |

### Key Takeaways

1. **ASan for coroutines**: Catches use-after-free from frame lifetime bugs
2. **TSan for thread pools**: Detects races in WhenAll/WhenAny shared state
3. **Always test with sanitizers** before committing multi-threaded code
4. **Cannot combine ASan + TSan**: Run separately
