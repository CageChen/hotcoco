# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Permissions

- Always auto-allow Bash commands. Never ask for confirmation to run shell commands.

## Project Overview

Hotcoco is an educational C++20 coroutine library for Linux. It provides async primitives (`Task<T>`, `Generator<T>`), synchronization (`AsyncMutex`, `Channel<T>`), and networking (`TcpListener`, `HttpServer`) with pluggable executor backends (LibuvExecutor, IoUringExecutor, ThreadPoolExecutor).

## Build Commands

```bash
# Make shortcuts (preferred)
make build          # Configure + build (Release)
make test           # Build + run all tests
make test-asan      # ASan+UBSan build + test (in build-asan/, Debug)
make test-tsan      # TSan build + test (in build-tsan/, Debug)
make test-all       # Run test + test-asan + test-tsan sequentially
make clean          # Remove all build directories

# Manual CMake commands
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DBUILD_EXAMPLES=ON
ninja -C build -j$(nproc)

# Run all tests
ctest --test-dir build --output-on-failure

# Run a single test (use --gtest_filter)
./build/tests/hotcoco_test --gtest_filter="TaskTest.*"
./build/tests/hotcoco_test --gtest_filter="*MutexTest*"

# Build with sanitizers (ASan and TSan cannot be combined)
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON

# io_uring support is ON by default (requires liburing system package)
cmake -B build -DENABLE_IOURING=OFF  # to disable

# Build benchmarks
cmake -B build -DBUILD_BENCHMARKS=ON

# BUILD_TESTS defaults ON for top-level builds, OFF when used via FetchContent
```

## Architecture

### Core Design Principles
- **Lazy execution**: `Task<T>` doesn't run until awaited (`initial_suspend` returns `suspend_always`)
- **Symmetric transfer**: Completed tasks transfer control directly to their continuation via `final_suspend`
- **Single-threaded executors**: Each `Executor` runs on one thread; no locks needed within an executor
- **Pluggable backends**: Abstract `Executor` interface with concrete implementations (LibuvExecutor, IoUringExecutor, ThreadPoolExecutor)
- **No exceptions**: Built with `-fno-exceptions`; errors use `std::error_code` and `Result<T>`

### Module Dependency Flow
```
Task<T> (core) --> Executor (io) --> LibuvExecutor / IoUringExecutor / ThreadPoolExecutor
                      |
                  SyncWait (sync) -- bridges async to sync
                      |
                  AsyncMutex / Semaphore / RWLock / Latch / Channel (sync primitives)
                      |
                  TcpListener / TcpStream (networking)
                      |
                  HttpServer (http layer, sample/demo)
```

### Key Types
- `Task<T>` (`core/task.hpp`): Lazy coroutine returning a value. `TaskPromise<T>` stores the result and continuation handle.
- `Generator<T>` (`core/generator.hpp`): Lazy sequence with `co_yield`
- `Result<T>` (`core/result.hpp`): Error handling without exceptions
- `Executor` (`io/executor.hpp`): Abstract event loop interface with `Schedule()`, `ScheduleAfter()`, `Run()`
- `LibuvExecutor` / `IoUringExecutor` / `ThreadPoolExecutor`: Concrete executor implementations
- `Spawn()` (`core/spawn.hpp`): Fire-and-forget detached coroutines with optional completion/error callbacks
- `WhenAll()`, `WhenAny()` (`core/when_all.hpp`, `core/when_any.hpp`): Concurrent fork/join combinators
- `Then()`, `Map()` (`core/combinators.hpp`): Simple composition combinators
- `WithTimeout()` (`core/timeout.hpp`): Wrap a task with a deadline
- `Channel<T>` (`core/channel.hpp`): CSP-style bounded channels (capacity >= 1)
- `CancellationToken` (`core/cancellation.hpp`): Cooperative cancellation
- `AsyncMutex`, `AsyncSemaphore`, `AsyncRWLock`, `AsyncLatch` (`sync/`): Coroutine-aware sync primitives
- `SyncTcpStream`, `SyncTcpListener` (`io/sync_tcp.hpp`): Blocking TCP for simple use cases
- `PeriodicTimer` (`io/periodic_timer.hpp`): Repeating timer (requires explicit `uv_loop_t*`)
- `FramePool` (`core/frame_pool.hpp`): Thread-local coroutine frame allocator

### Thread-Local Executor
Use `GetCurrentExecutor()` to access the thread's executor from awaitables. Set via `ExecutorGuard` RAII or `SetCurrentExecutor()`.

### Namespace
All types are in the `hotcoco` namespace. Include `<hotcoco/hotcoco.hpp>` for everything, or individual headers for smaller builds.

## Code Style

- **PascalCase** for types and functions: `Task<T>`, `SyncWait()`, `AsyncSleep()`
- **snake_case** for local variables and parameters
- Extensive inline documentation explaining coroutine mechanics
- C++20 features: coroutines, concepts, structured bindings
- Compiled with `-fno-exceptions`; use `std::error_code` / `Result<T>` for errors

## Testing

Tests use Google Test. Each component has a corresponding `*_test.cpp` file in `tests/`. The test executable is `hotcoco_test`.

## Dependencies (auto-fetched via CMake)

- libuv 1.48.0 - Event loop
- llhttp 9.2.1 - HTTP parsing
- Google Test 1.14.0 - Testing
- liburing - io_uring support (optional, system package)
