# Hotcoco 🍫

**A modern C++20 coroutine library for Linux — async made simple.**

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Linux](https://img.shields.io/badge/platform-Linux-green.svg)]()
[![License: MIT](https://img.shields.io/badge/license-MIT-yellow.svg)](LICENSE)
[![Tests](https://img.shields.io/badge/tests-365%20passed-brightgreen.svg)]()

Hotcoco brings the ergonomics of modern async/await to C++20 with **zero exceptions**, **pluggable executors**, and **coroutine-native synchronization** — all in a single, well-documented library.

```cpp
#include <hotcoco/hotcoco.hpp>
using namespace hotcoco;

Task<void> Greet(std::string name) {
    co_await AsyncSleep(std::chrono::milliseconds(100));
    std::cout << "Hello, " << name << "!" << std::endl;
}

int main() {
    auto executor = LibuvExecutor::Create().Value();
    auto task = Greet("world");
    executor->Schedule(task.GetHandle());
    executor->Run();  // Hello, world!
}
```

## Why Hotcoco?

- **Modern C++20** — built entirely on coroutines, concepts, and structured bindings
- **No exceptions** — `Result<T>` and `std::error_code` everywhere, compiled with `-fno-exceptions`
- **Pluggable executors** — libuv, io_uring, thread pool, or bring your own
- **Batteries included** — mutex, semaphore, channels, retry, timeout, cancellation
- **Educational** — 12 tutorials and extensive inline documentation explaining the *why*, not just the *how*

## Features

### Core Primitives
| Feature | Description |
|---------|-------------|
| **`Task<T>`** | Lazy coroutine with symmetric transfer |
| **`Generator<T>`** | Lazy sequence generator with `co_yield` |
| **`Result<T>`** | Rust-inspired error handling with `Map`, `AndThen`, `OrElse` |
| **`Spawn()`** | Detached fire-and-forget coroutines with callbacks |
| **`Defer`** | Go-style cleanup on scope exit |

### Combinators & Control Flow
| Feature | Description |
|---------|-------------|
| **`WhenAll()`** | Concurrent fork/join with atomic countdown latch |
| **`WhenAny()`** | First-completion race with atomic CAS |
| **`WithTimeout()`** | Deadline-based cancellation |
| **`Retry()`** | Automatic retry with exponential backoff + jitter |
| **`Then()` / `Map()`** | Composable task pipelines |
| **`CancellationToken`** | Cooperative cancellation with callbacks |

### Synchronization
| Feature | Description |
|---------|-------------|
| **`AsyncMutex`** | Coroutine-aware mutual exclusion |
| **`AsyncSemaphore`** | Counting semaphore for concurrency limiting |
| **`AsyncRWLock`** | Fair reader-writer lock |
| **`AsyncLatch`** | One-shot countdown barrier |
| **`Channel<T>`** | CSP-style bounded channels with direct handoff |
| **`SyncWait()`** | Bridge sync → async |

### I/O & Networking
| Feature | Description |
|---------|-------------|
| **`LibuvExecutor`** | Single-threaded event loop (libuv/epoll) |
| **`IoUringExecutor`** | High-performance io_uring executor (Linux 5.1+) |
| **`ThreadPoolExecutor`** | Multi-threaded work distribution |
| **`TcpListener` / `TcpStream`** | Async TCP networking |
| **`HttpServer`** | HTTP/1.1 server with llhttp parser |
| **`AsyncSleep` / `PeriodicTimer`** | Non-blocking timers |

### Performance
| Feature | Description |
|---------|-------------|
| **`FramePool`** | Thread-local coroutine frame recycling |
| **CPU Affinity** | Thread pinning and configuration |
| **Allocator Options** | tcmalloc / jemalloc / mimalloc support |
| **Sanitizers** | ASan, TSan, UBSan build integration |

## Quick Start

### Async TCP Echo Server

```cpp
Task<void> HandleClient(std::unique_ptr<TcpStream> client) {
    while (client->IsOpen()) {
        auto data = co_await client->Read();
        if (data.empty()) break;
        co_await client->Write(std::string_view(data.data(), data.size()));
    }
}

Task<void> RunServer(LibuvExecutor& executor) {
    TcpListener listener(executor);
    listener.Bind("127.0.0.1", 8080);
    listener.Listen();

    while (true) {
        auto client = co_await listener.Accept();
        Spawn(executor, HandleClient(std::move(client)));
    }
}
```

### Concurrent Tasks

```cpp
Task<void> FetchAll() {
    // Run three tasks concurrently, wait for all
    auto [a, b, c] = co_await WhenAll(
        FetchUser(1),
        FetchUser(2),
        FetchUser(3)
    );

    // Race: first result wins
    auto winner = co_await WhenAny(FastAPI(), SlowAPI());
    // winner is Result<WhenAnyResult<T>, std::error_code>
    std::cout << "Task " << winner.Value().index << " won" << std::endl;
}
```

### Retry with Backoff

```cpp
auto result = co_await Retry([]() {
    return FetchFromAPI();
}).MaxAttempts(5)
  .WithExponentialBackoff(100ms, 2.0)
  .MaxDelay(30s)
  .RetryIf([](std::error_code ec) {
      return ec.value() == ECONNREFUSED;
  })
  .Execute();
```

### Timeout

```cpp
auto result = co_await WithTimeout(SlowOperation(), 5s);
if (result.IsErr()) {
    std::cerr << result.Error().Message() << std::endl;
}
```

### Channels

```cpp
Channel<int> ch(10);
co_await ch.Send(42);
auto val = co_await ch.Receive();  // std::optional<int>
if (val) {
    std::cout << *val << std::endl;  // 42
}
```

### HTTP Server

```cpp
int main() {
    auto executor = LibuvExecutor::Create().Value();

    HttpServer server(*executor, "127.0.0.1", 8080);
    server.OnRequest([](const HttpRequest& req) {
        if (req.path == "/")
            return HttpResponse::Html("<h1>Hello, hotcoco!</h1>");
        return HttpResponse::NotFound();
    });

    auto task = server.Run();
    executor->Schedule(task.GetHandle());
    executor->Run();
}
```

## Tutorials

Step-by-step guides covering the *what*, *how*, and *why* of each component:

1. [Coroutine Fundamentals](docs/tutorials/01_coroutine_fundamentals.md) — Task, Generator, SyncWait
2. [Event Loops and libuv](docs/tutorials/02_event_loops_and_libuv.md) — Executor, LibuvExecutor, AsyncSleep
3. [Async TCP Networking](docs/tutorials/03_async_tcp_networking.md) — TcpListener, TcpStream
4. [HTTP Server](docs/tutorials/04_http_server.md) — HttpParser, HttpResponse, routing
5. [Multi-Threaded Executors](docs/tutorials/05_multithreaded_executors.md) — ThreadPoolExecutor, WhenAll, WhenAny
6. [Debugging with Sanitizers](docs/tutorials/06_debugging_with_sanitizers.md) — ASan, TSan, UBSan
7. [Error Handling with Result](docs/tutorials/07_error_handling_with_result.md) — Result\<T\>, combinators
8. [Cancellation and Timeouts](docs/tutorials/08_cancellation_and_timeouts.md) — CancellationToken, WithTimeout
9. [Spawn and Detached Coroutines](docs/tutorials/09_spawn_and_detached_coroutines.md) — fire-and-forget patterns
10. [Synchronization Primitives](docs/tutorials/10_synchronization_primitives.md) — Mutex, Semaphore, RWLock
11. [Retry and Error Recovery](docs/tutorials/11_retry_and_error_recovery.md) — exponential backoff
12. [io_uring Executor](docs/tutorials/12_iouring_executor.md) — high-performance Linux I/O

## Building

```bash
# Quick start
make build          # Configure + build (Release)
make test           # Build + run all 365 tests
make test-asan      # ASan+UBSan build + test
make test-tsan      # TSan build + test
make clean          # Remove all build directories

# Or manually
cmake -B build -G Ninja -DBUILD_TESTS=ON -DBUILD_EXAMPLES=ON
ninja -C build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_TESTS` | ON | Build unit tests (365 tests across 33 files) |
| `BUILD_EXAMPLES` | ON | Build example programs |
| `BUILD_BENCHMARKS` | OFF | Build performance benchmarks |
| `ENABLE_IOURING` | ON | Enable io_uring executor (requires liburing) |
| `ENABLE_ASAN` | OFF | AddressSanitizer |
| `ENABLE_TSAN` | OFF | ThreadSanitizer |
| `ENABLE_UBSAN` | OFF | UndefinedBehaviorSanitizer |

## Dependencies

All dependencies are **auto-fetched** via CMake FetchContent:

| Dependency | Version | Purpose |
|------------|---------|---------|
| libuv | 1.48.0 | Event loop |
| llhttp | 9.2.1 | HTTP parsing |
| Google Test | 1.14.0 | Testing |
| liburing | system | io_uring support (optional) |

**Compiler**: GCC 11+ or Clang 14+ with C++20 support.

## License

MIT
