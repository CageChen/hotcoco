# Tutorial 12: High-Performance I/O with io_uring

This tutorial introduces the `IoUringExecutor`, a high-performance alternative to `LibuvExecutor` for modern Linux systems.

## Table of Contents

1. [What is io_uring?](#what-is-io_uring)
2. [Prerequisites](#prerequisites)
3. [Using IoUringExecutor](#using-iouringexecutor)
4. [LibuvExecutor vs IoUringExecutor](#libuvexecutor-vs-iouringexecutor)
5. [When to Use Which](#when-to-use-which)

---

## What is io_uring?

**io_uring** is a modern Linux kernel interface (5.1+) for asynchronous I/O. It provides:

| Feature | Benefit |
|---------|---------|
| **Submission Queue** | Batch multiple I/O operations in one syscall |
| **Completion Queue** | Poll for completions without blocking |
| **Registered Buffers** | Zero-copy I/O for networking |
| **Minimal Overhead** | No callback allocations or context switches |

### Traditional vs io_uring

```
Traditional (epoll):
├── syscall: epoll_wait()      ← Block waiting
├── syscall: read()            ← Perform I/O
├── syscall: read()            ← More I/O
├── syscall: epoll_ctl()       ← Modify interest
└── syscall: epoll_wait()      ← Block again

io_uring:
├── Submit: [read, read, timeout]  ← Batch submission
├── syscall: io_uring_enter()      ← Single syscall
└── Poll: [completion, completion] ← Non-blocking check
```

---

## Prerequisites

### Kernel Version

io_uring requires Linux kernel 5.1 or later. Check with:

```bash
uname -r
# Should be >= 5.1
```

### Install liburing

```bash
# Ubuntu/Debian
sudo apt install liburing-dev

# Fedora/RHEL
sudo dnf install liburing-devel

# Build from source
git clone https://github.com/axboe/liburing.git
cd liburing && ./configure && make && sudo make install
```

### Build hotcoco with io_uring

```bash
mkdir build && cd build
cmake .. -G Ninja -DENABLE_IOURING=ON
ninja
```

Check if io_uring is enabled:
```bash
grep 'HOTCOCO_HAS_IOURING' CMakeCache.txt
# Should show: HOTCOCO_HAS_IOURING:INTERNAL=1
```

---

## Using IoUringExecutor

### Basic Usage

```cpp
#include <hotcoco/io/iouring_executor.hpp>
#include <hotcoco/core/task.hpp>
#include <hotcoco/io/timer.hpp>

using namespace hotcoco;

#ifdef HOTCOCO_HAS_IOURING

Task<void> Example() {
    std::cout << "Starting..." << std::endl;
    co_await AsyncSleep(100ms);  // Uses IORING_OP_TIMEOUT
    std::cout << "100ms later!" << std::endl;
}

int main() {
    auto executor_result = IoUringExecutor::Create();
    auto& executor = *executor_result.Value();

    auto task = Example();
    executor.Schedule(task.GetHandle());
    executor.Run();
}

#endif
```

### Constructor Options

```cpp
// Default: 256 queue depth
auto executor = IoUringExecutor::Create();

// Custom queue depth for high-concurrency workloads
auto executor = IoUringExecutor::Create(1024);
```

---

## LibuvExecutor vs IoUringExecutor

| Aspect | LibuvExecutor | IoUringExecutor |
|--------|---------------|-----------------|
| **Kernel** | Any Linux | Linux 5.1+ |
| **Syscalls** | Per-operation | Batched |
| **Cross-platform** | Yes (via libuv) | Linux only |
| **Maturity** | Very stable | Modern, evolving |
| **Best for** | General use, portability | Maximum performance |

### Performance Characteristics

**LibuvExecutor** (epoll-based):
- One syscall per I/O operation
- Callback-driven (libuv internal)
- Proven, stable behavior

**IoUringExecutor**:
- Batched syscalls reduce overhead
- Direct completion queue polling
- Better cache locality
- ~10-30% lower latency in I/O-heavy workloads

---

## When to Use Which

### Use LibuvExecutor when:
- Portability matters (planning macOS/Windows support)
- Running on older Linux kernels (< 5.1)
- Simple applications where I/O isn't the bottleneck

### Use IoUringExecutor when:
- Maximum performance is critical
- Running on modern Linux (5.1+)
- High-concurrency network servers
- Database or storage-heavy applications

### Feature Detection Pattern

```cpp
#include <hotcoco/io/executor.hpp>
#include <hotcoco/io/libuv_executor.hpp>

#ifdef HOTCOCO_HAS_IOURING
#include <hotcoco/io/iouring_executor.hpp>
#endif

std::unique_ptr<Executor> CreateBestExecutor() {
#ifdef HOTCOCO_HAS_IOURING
    auto result = IoUringExecutor::Create();
    if (result.IsOk()) return std::move(result.Value());
#endif
    return LibuvExecutor::Create().Value();
}
```

---

## How IoUringExecutor Works

### Architecture

```
┌─────────────────────────────────────────────────────────┐
│                   IoUringExecutor                       │
├─────────────────────────────────────────────────────────┤
│  io_uring ring_          ← Submission/Completion queues │
│  int eventfd_            ← Cross-thread wakeup          │
├─────────────────────────────────────────────────────────┤
│  std::queue<coroutine_handle<>> ready_queue_            │
│  std::mutex queue_mutex_ ← Thread-safe scheduling       │
└─────────────────────────────────────────────────────────┘
```

### Scheduling Flow

1. **Schedule()** pushes handle to ready queue, writes to eventfd
2. **Run()** waits on io_uring CQ with eventfd read submitted
3. When eventfd completes, executor processes ready queue
4. Coroutines are resumed, may schedule more work

### Timer Implementation

```cpp
void ScheduleAfter(milliseconds delay, coroutine_handle<> h) {
    // Use IORING_OP_TIMEOUT for efficient kernel-level timer
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    io_uring_prep_timeout(sqe, &timespec, 0, 0);
    io_uring_sqe_set_data(sqe, context);
    io_uring_submit(&ring_);
}
```

---

## Summary

| Concept | Purpose |
|---------|---------|
| io_uring | Modern Linux async I/O interface |
| IoUringExecutor | hotcoco Executor using io_uring |
| eventfd | Thread-safe wakeup mechanism |
| IORING_OP_TIMEOUT | Kernel-level timer for AsyncSleep |

### Key Takeaways

1. **io_uring reduces syscall overhead** through batching
2. **IoUringExecutor is a drop-in replacement** for LibuvExecutor
3. **Use feature detection** for portable code
4. **Linux 5.1+ required** for io_uring support

---

## Next Steps

With io_uring support, hotcoco is ready for high-performance Linux applications. Future enhancements may include:

- io_uring-based TCP networking (IORING_OP_ACCEPT, RECV, SEND)
- Zero-copy receive (ZCRX) for ultimate network performance
- io_uring-based file I/O
