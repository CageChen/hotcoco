# AGENTS.md

Development guidelines for the hotcoco repository. All AI agents should read this file before making any code changes.

## Dev Environment

- **Language**: C++20 (coroutines, concepts, structured bindings)
- **Compiler**: GCC 11+ or Clang 14+
- **Build System**: CMake + Ninja
- **Platform**: Linux only
- **Compiler Flags**: `-fno-exceptions`; errors are handled via `std::error_code` / `Result<T>`
- **Dependencies**: All fetched automatically via CMake FetchContent (libuv 1.48.0, llhttp 9.2.1, Google Test 1.14.0)
- **Optional Dependencies**: liburing (system package, `apt install liburing-dev`)
- **Memory Allocators**: Optional support for tcmalloc / jemalloc / mimalloc

## Build Commands

```bash
make build          # Release build (build/)
make test           # Build + run all tests
make test-asan      # ASan+UBSan build + test (build-asan/, Debug)
make test-tsan      # TSan build + test (build-tsan/, Debug)
make test-all       # Run test + test-asan + test-tsan sequentially
make coverage       # Coverage build + test + lcov HTML report (build-coverage/)
make clean          # Clean all build directories
make format         # clang-format all source files
make check-format   # Check formatting (CI, exits 1 on failure)
make lint           # clang-tidy static analysis
make setup          # Configure git hooks (.githooks/pre-commit)
```

Run a specific test:
```bash
./build/tests/hotcoco_test --gtest_filter="TaskTest.*"
```

Key CMake options:

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_TESTS` | ON | Build unit tests |
| `BUILD_EXAMPLES` | ON | Build example programs |
| `BUILD_BENCHMARKS` | OFF | Build benchmarks |
| `ENABLE_IOURING` | ON | Enable io_uring executor |
| `ENABLE_COVERAGE` | OFF | Code coverage (gcov + lcov) |
| `ENABLE_ASAN` | OFF | AddressSanitizer |
| `ENABLE_TSAN` | OFF | ThreadSanitizer |
| `ENABLE_UBSAN` | OFF | UndefinedBehaviorSanitizer |

## Testing

- **Framework**: Google Test 1.14.0
- **Test Files**: One `*_test.cpp` per component under `tests/`
- **Executable**: `build/tests/hotcoco_test`
- **CI**: GitHub Actions (`.github/workflows/ci.yml`) with 5 jobs:
  - `build-and-test`: Release build + test
  - `asan`: ASan+UBSan build + test
  - `clang-tidy`: Static analysis
  - `format-check`: clang-format-19 formatting check
  - `coverage`: gcov + lcov coverage report (uploaded as artifact)

## Code Style

- **Naming**: PascalCase for types and functions, snake_case for local variables and parameters
- **Formatting**: clang-format (Google-based, 4-space indent, 120-column limit)
- **Static Analysis**: clang-tidy (bugprone, clang-analyzer, cppcoreguidelines, modernize, performance, readability)
- **Editor**: `.editorconfig` (spaces, UTF-8, trailing whitespace trimming)
- **Git Hook**: pre-commit hook auto-checks clang-format on staged files
- **Error Handling**: Exceptions are disabled (`-fno-exceptions`); use `Result<T>` and `std::error_code`
- **Annotations**: Functions returning `Task`, `Result`, `Generator`, or `SpawnHandle` should be marked `[[nodiscard]]`

## Architecture

```
include/hotcoco/
├── hotcoco.hpp           # Umbrella header, includes all modules
├── core/                 # Core coroutine primitives
│   ├── task.hpp          # Task<T>: lazy coroutine with symmetric transfer
│   ├── generator.hpp     # Generator<T>: lazy sequence generator
│   ├── result.hpp        # Result<T>: Rust-style error handling
│   ├── spawn.hpp         # Spawn(): fire-and-forget coroutine
│   ├── when_all.hpp      # WhenAll(): concurrent fork/join
│   ├── when_any.hpp      # WhenAny(): first-to-complete
│   ├── combinators.hpp   # Then()/Map(): combinators
│   ├── timeout.hpp       # WithTimeout(): timeout control
│   ├── retry.hpp         # Retry(): exponential backoff retry
│   ├── cancellation.hpp  # CancellationToken: cooperative cancellation
│   ├── channel.hpp       # Channel<T>: CSP-style bounded channel
│   ├── frame_pool.hpp    # FramePool: thread-local coroutine frame pool
│   ├── task_group.hpp    # TaskGroup: structured task group
│   ├── defer.hpp         # Defer: Go-style scope cleanup
│   ├── error.hpp         # Error code definitions
│   ├── concepts.hpp      # C++20 concepts
│   ├── invoke.hpp        # Coroutine invocation helpers
│   ├── schedule_on.hpp   # Cross-executor scheduling
│   ├── detached_task.hpp # Detached task
│   ├── check.hpp         # Assertion macros
│   └── coroutine_compat.hpp # Coroutine compatibility layer
├── io/                   # I/O and Executors
│   ├── executor.hpp      # Executor abstract interface
│   ├── libuv_executor.hpp     # LibuvExecutor (libuv/epoll)
│   ├── iouring_executor.hpp   # IoUringExecutor (io_uring)
│   ├── thread_pool_executor.hpp # ThreadPoolExecutor
│   ├── polling_executor.hpp   # PollingExecutor (polling-based)
│   ├── tcp.hpp           # TcpListener/TcpStream (async TCP)
│   ├── iouring_tcp.hpp   # io_uring TCP implementation
│   ├── sync_tcp.hpp      # SyncTcpStream/SyncTcpListener (blocking TCP)
│   ├── timer.hpp         # AsyncSleep / timers
│   ├── periodic_timer.hpp # PeriodicTimer
│   ├── iouring_async_ops.hpp  # io_uring async operations
│   ├── iouring_file.hpp      # io_uring RAII file I/O
│   └── thread_utils.hpp  # CPU affinity / thread utilities
├── sync/                 # Synchronization primitives
│   ├── mutex.hpp         # AsyncMutex
│   ├── semaphore.hpp     # AsyncSemaphore
│   ├── rwlock.hpp        # AsyncRWLock
│   ├── latch.hpp         # AsyncLatch
│   ├── event.hpp         # AsyncEvent
│   └── sync_wait.hpp     # SyncWait(): synchronously wait for async task
├── execution/            # C++23 std::execution adapters
│   ├── scheduler.hpp     # Scheduler adapter
│   ├── sender.hpp        # Sender adapter
│   ├── as_awaitable.hpp  # Sender → Awaitable bridge
│   └── stop_token_adapter.hpp # stop_token adapter
└── http/
    └── http.hpp          # HttpServer/HttpRequest/HttpResponse
```

Source files are under `src/`, organized into `core/`, `io/`, and `http/` directories, mirroring the header structure.

### Key Design Principles

- **Lazy Execution**: `Task<T>` does not execute until `co_await`ed
- **Symmetric Transfer**: Coroutines transfer directly to their continuation via `final_suspend`
- **Single-Threaded Executor**: Each Executor runs on a single thread; no locking needed within an executor
- **Thread-Local Executor**: Use `GetCurrentExecutor()` to obtain the current thread's executor

## Code Review

- **Commit Messages**: Concise and clear; use imperative mood for the subject line
- **Pre-commit Hook**: Run `make setup` to configure git hooks; staged files are auto-checked with clang-format before commit
- **CI Checks**: Every PR must pass all 4 CI jobs: build+test, ASan, clang-tidy, and format-check
- **Sanitizers**: Run `make test-asan` locally before submitting to catch memory errors early
