# Tutorial 13: Bridging hotcoco with P2300 std::execution

This tutorial explains what P2300 `std::execution` is, why it matters, and how hotcoco provides a thin adapter layer to interoperate with the Senders/Receivers ecosystem.

## Table of Contents

1. [What is P2300?](#what-is-p2300)
2. [Core Concepts: Scheduler, Sender, Receiver](#core-concepts-scheduler-sender-receiver)
3. [Building with stdexec](#building-with-stdexec)
4. [Executor → Scheduler Adapter](#executor--scheduler-adapter)
5. [Task\<T\> → Sender Adapter](#taskt--sender-adapter)
6. [Sender → co_await Bridge](#sender--co_await-bridge)
7. [stop_token ↔ CancellationToken Bridge](#stop_token--cancellationtoken-bridge)
8. [Combining Everything](#combining-everything)
9. [Implementation Deep Dive](#implementation-deep-dive)

---

## What is P2300?

**P2300** is the C++ proposal (targeted for C++26) that introduces `std::execution` — a structured, composable framework for asynchronous programming based on the **Senders/Receivers** model.

### The Problem P2300 Solves

C++ has no standard way to compose async operations. Every library invents its own:

| Library | Async Model |
|---------|-------------|
| Boost.Asio | Completion handlers / coroutines |
| libunifex | Senders/Receivers (precursor to P2300) |
| hotcoco | Coroutines + Executor |
| folly | Futures/Promises |

P2300 provides a **single vocabulary** so that async libraries can interoperate — a sender from one library can be consumed by algorithms from another.

### Coroutines vs Senders/Receivers

hotcoco uses **coroutines** as its primary async model:

```cpp
// hotcoco style: coroutines
Task<int> Compute() {
    int a = co_await FetchA();
    int b = co_await FetchB();
    co_return a + b;
}
```

P2300 uses **senders** as its primary model:

```cpp
// P2300 style: sender pipelines
auto compute = stdexec::just()
             | stdexec::let_value([] { return FetchA(); })
             | stdexec::let_value([](int a) {
                   return FetchB()
                        | stdexec::then([a](int b) { return a + b; });
               });
```

Neither model is "better" — they serve different needs. Coroutines are more natural for sequential control flow; senders excel at static composition and dataflow pipelines. The hotcoco adapter layer lets you use **both models together**.

---

## Core Concepts: Scheduler, Sender, Receiver

P2300 is built on three fundamental concepts:

### Sender

A **sender** describes an async operation that hasn't started yet. It's lazy — just a recipe for work.

```
Sender = "I will eventually produce values, an error, or be cancelled"
```

Every sender declares its **completion signatures** — the ways it can complete:

```cpp
// This sender completes with set_value(int) or set_stopped()
using signatures = stdexec::completion_signatures<
    stdexec::set_value_t(int),    // Success: produces an int
    stdexec::set_stopped_t()      // Cancellation
>;
```

### Receiver

A **receiver** consumes the result of a sender. It has three channels:

| Channel | Purpose |
|---------|---------|
| `set_value(args...)` | Success — deliver result values |
| `set_error(e)` | Failure — deliver an error |
| `set_stopped()` | Cancellation — operation was cancelled |

### Operation State & The connect/start Protocol

Senders and receivers are connected via a two-step protocol:

```
1. connect(sender, receiver)  →  operation_state
2. start(operation_state)     →  begins the async work
```

```cpp
auto sender = stdexec::just(42);              // Sender: will produce 42
auto op = stdexec::connect(sender, receiver); // Wire them together
stdexec::start(op);                           // Go!
// Eventually: receiver.set_value(42) is called
```

### Scheduler

A **scheduler** represents an execution context (thread pool, event loop, etc.). Calling `schedule()` on a scheduler returns a sender that completes "on" that context:

```cpp
auto sender = stdexec::schedule(scheduler)         // Transfer to scheduler
            | stdexec::then([] { return 42; });     // Run this there
```

### Composition with Pipe Operator

P2300 senders compose with `|`, just like Unix pipes:

```cpp
auto pipeline = stdexec::just(21)                     // Start with 21
              | stdexec::then([](int x) { return x * 2; })  // Double it
              | stdexec::then([](int x) { return x + 1; }); // Add 1
// Result: 43
```

---

## Building with stdexec

hotcoco uses [NVIDIA/stdexec](https://github.com/NVIDIA/stdexec) as the P2300 reference implementation. The adapter layer is behind a compile flag:

```bash
# Build with stdexec support
cmake -B build-stdexec -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=ON \
    -DENABLE_STDEXEC=ON

ninja -C build-stdexec
```

The flag `ENABLE_STDEXEC` (default OFF) does two things:
1. Fetches stdexec via CMake FetchContent
2. Defines `HOTCOCO_HAS_STDEXEC=1` for conditional compilation

Your existing code is **completely unaffected** when the flag is OFF.

### Feature Detection in Code

```cpp
#ifdef HOTCOCO_HAS_STDEXEC
#include <hotcoco/execution/scheduler.hpp>
#include <hotcoco/execution/sender.hpp>
#include <hotcoco/execution/as_awaitable.hpp>
#include <hotcoco/execution/stop_token_adapter.hpp>
#endif
```

---

## Executor → Scheduler Adapter

hotcoco's `Executor` is an event loop with `Run()`, `Stop()`, `Post()`, and `Schedule()`. P2300 expects a `scheduler` with a `schedule()` method returning a sender. The adapter bridges this gap.

### Usage

```cpp
#include <hotcoco/execution/scheduler.hpp>
#include <stdexec/execution.hpp>

using namespace hotcoco;

auto exec = LibuvExecutor::Create().Value();
auto scheduler = execution::AsScheduler(*exec);

// Now use P2300 algorithms!
auto sender = stdexec::schedule(scheduler)
            | stdexec::then([] { return 42; });

auto result = stdexec::sync_wait(std::move(sender));
auto [value] = *result;
// value == 42
```

### How It Works

The adapter consists of three types:

```
AsScheduler(executor)
    └── HotcocoScheduler         (satisfies stdexec::scheduler)
            │
            └── schedule()
                    └── ScheduleSender   (satisfies stdexec::sender)
                            │
                            └── connect(receiver)
                                    └── ScheduleOperation<Receiver>
                                            │
                                            └── start()
                                                    └── executor->Post(callback)
                                                            └── set_value(receiver)
```

When `start()` is called on the operation state, it posts a callback to the executor via `Post()`. The callback runs on the executor's event loop thread and calls `set_value` on the receiver — completing the sender.

### Stop Token Support

The scheduler respects P2300 cancellation. If the receiver's stop token has been triggered before the callback runs, `set_stopped()` is called instead:

```cpp
void start() noexcept {
    executor_->Post([this] {
        if constexpr (stdexec::unstoppable_token</* ... */>) {
            stdexec::set_value(std::move(receiver_));
        } else if (stdexec::get_stop_token(stdexec::get_env(receiver_))
                       .stop_requested()) {
            stdexec::set_stopped(std::move(receiver_));
        } else {
            stdexec::set_value(std::move(receiver_));
        }
    });
}
```

---

## Task\<T\> → Sender Adapter

`Task<T>` is hotcoco's core async primitive — a lazy, move-only coroutine. The sender adapter lets you plug any `Task<T>` into P2300 pipelines.

### Usage

```cpp
#include <hotcoco/execution/sender.hpp>

Task<int> ComputeAsync() { co_return 42; }

// Wrap as sender and compose with P2300 algorithms
auto sender = execution::AsSender(ComputeAsync())
            | stdexec::then([](int x) { return x * 2; });

auto result = stdexec::sync_wait(std::move(sender));
auto [value] = *result;
// value == 84
```

### Void Tasks

```cpp
bool executed = false;
auto make_task = [&]() -> Task<void> {
    executed = true;
    co_return;
};

auto sender = execution::AsSender(make_task());
stdexec::sync_wait(std::move(sender));
// executed == true
```

### Multi-Stage Pipelines

```cpp
Task<int> GetBase() { co_return 10; }

auto sender = execution::AsSender(GetBase())
            | stdexec::then([](int x) { return x + 5; })   // 15
            | stdexec::then([](int x) { return x * 2; });  // 30

auto [result] = *stdexec::sync_wait(std::move(sender));
// result == 30
```

### How It Works: The Bridge Coroutine

The key challenge is: `Task<T>` is a coroutine that needs to be `co_await`ed, but P2300's `start()` is a regular function. The solution is a **bridge coroutine**:

```cpp
// Eager fire-and-forget coroutine (suspend_never initial/final suspend)
template <typename T>
struct BridgeTask {
    struct promise_type {
        BridgeTask get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::abort(); }
    };
};
```

When `start()` is called on the operation state, it launches this bridge coroutine:

```cpp
void start() noexcept {
    // Both task and receiver are passed by value so they live
    // in the coroutine frame, not via `this`.
    run_bridge(std::move(task_), std::move(receiver_));
}

static auto run_bridge(Task<T> t, Receiver rcvr) -> detail::BridgeTask<T> {
    if constexpr (std::is_void_v<T>) {
        co_await std::move(t);
        stdexec::set_value(std::move(rcvr));
    } else {
        auto result = co_await std::move(t);
        stdexec::set_value(std::move(rcvr), std::move(result));
    }
}
```

The bridge coroutine starts eagerly (no suspension), `co_await`s the task, and when the task completes, delivers the result to the receiver via `set_value`.

> [!IMPORTANT]
> Both `task` and `receiver` are passed **by value** into the coroutine frame. This is critical — if the bridge coroutine accessed them via `this`, the `TaskSenderOperation` might be destroyed before the coroutine resumes from an async `Task`, causing use-after-free.

### Completion Signatures

`TaskSender<T>` declares what it can produce:

```cpp
template <class, class...>
static consteval auto get_completion_signatures() noexcept {
    if constexpr (std::is_void_v<T>) {
        return stdexec::completion_signatures<set_value_t()>{};
    } else {
        return stdexec::completion_signatures<set_value_t(T)>{};
    }
}
```

Note: hotcoco uses `-fno-exceptions`, so there is no `set_error(exception_ptr)` channel. Errors are conveyed through `Result<T>` in the value channel.

---

## Sender → co_await Bridge

The reverse direction: take any P2300 sender and `co_await` it inside a hotcoco coroutine.

### Usage

```cpp
#include <hotcoco/execution/as_awaitable.hpp>

Task<int> MyCoroutine() {
    // Build a P2300 sender pipeline
    auto sender = stdexec::just(21)
                | stdexec::then([](int x) { return x * 2; });

    // co_await it inside a hotcoco coroutine!
    int result = co_await execution::AsAwaitable(std::move(sender));
    co_return result;  // 42
}

int result = SyncWait(MyCoroutine());
```

### Void Senders

```cpp
Task<void> MyCoroutine() {
    auto sender = stdexec::just();
    co_await execution::AsAwaitable(std::move(sender));
    // Sender completed successfully
}
```

### How It Works

`SenderAwaitable<Sender>` implements the awaiter interface:

1. **`await_ready()`** — returns `false` (always suspend)
2. **`await_suspend(cont)`** — connects the sender to an internal `BridgeReceiver`, starts the operation
3. **`await_resume()`** — returns the stored result

The `BridgeReceiver` stores the result and resumes the coroutine:

```cpp
template <typename T>
struct BridgeReceiver {
    using receiver_concept = stdexec::receiver_t;
    AwaitableState<T>* state;

    void set_value(T val) noexcept {
        state->result.emplace(std::move(val));
        state->continuation.resume();       // Resume the awaiting coroutine
    }

    void set_stopped() noexcept {
        state->stopped = true;
        state->continuation.resume();
    }

    void set_error(auto&&) noexcept {
        std::abort();                        // -fno-exceptions: unrecoverable
    }
};
```

### Dealing with Immovable Operation States

P2300 operation states are **immovable** (deleted move constructor). This creates a challenge: how do you store an immovable type that you can only construct inside `await_suspend`?

The solution uses a **union** with **raw placement-new** to enable guaranteed copy elision. `SenderAwaitable` itself is also **non-movable** — since the operation state cannot be moved, the awaitable that contains it cannot be either:

```cpp
class SenderAwaitable {
    union {
        op_state_t op_;      // Deferred construction in union
    };
    bool has_op_;

    // Non-movable: op_state_t is immovable, and moving after
    // await_suspend would leave the operation state at stale memory.
    SenderAwaitable(SenderAwaitable&&) = delete;
    SenderAwaitable& operator=(SenderAwaitable&&) = delete;

    void await_suspend(std::coroutine_handle<> cont) noexcept {
        state_.continuation = cont;
        // Raw placement-new with prvalue enables guaranteed copy elision
        // (C++17 [dcl.init]/17.6.1)
        ::new (static_cast<void*>(&op_)) op_state_t(
            stdexec::connect(std::move(sender_), receiver_t{&state_}));
        has_op_ = true;
        stdexec::start(op_);
    }

    T await_resume() noexcept {
        HOTCOCO_CHECK(!state_.stopped, "sender was stopped");
        return std::move(*state_.result);
    }

    ~SenderAwaitable() {
        if (has_op_) {
            op_.~op_state_t();
        }
    }
};
```

Why not `std::optional` or `std::construct_at`?
- `std::optional::emplace()` requires `T` to be move-constructible
- `std::construct_at` has a SFINAE constraint that checks move-constructibility
- Raw placement-new with a prvalue directly constructs in-place — no move needed

> [!NOTE]
> C++17 guaranteed copy elision means `co_await AsAwaitable(std::move(sender))` directly constructs the `SenderAwaitable` in the coroutine frame — no move constructor needed.

---

## stop_token ↔ CancellationToken Bridge

P2300 uses `std::stop_token` for cancellation; hotcoco uses `CancellationToken`. The bridge provides bidirectional conversion.

### std::stop_token → CancellationToken

```cpp
#include <hotcoco/execution/stop_token_adapter.hpp>

std::stop_source ss;
auto [token, bridge] = execution::FromStopToken(ss.get_token());

token.IsCancelled();   // false

ss.request_stop();     // Trigger the stop_token

token.IsCancelled();   // true — automatically propagated!
```

The `bridge` (a `shared_ptr<StopTokenBridge>`) must be kept alive. It holds a `std::stop_callback` that calls `Cancel()` on an internal `CancellationSource` when the stop token fires.

### CancellationToken → std::stop_source

```cpp
CancellationSource source;
std::stop_source ss;

{
    auto link = execution::LinkCancellation(source.GetToken(), ss);

    source.Cancel();
    ss.stop_requested();   // true — automatically propagated!
}
// link destroyed — callback unregistered
// Future cancellations won't affect ss
```

### When Is This Useful?

When a P2300 algorithm cancels a sender that wraps hotcoco internals, or vice versa. For example, in the scheduler adapter, the receiver carries a `stop_token` from the P2300 side. If we needed to pass that cancellation into a hotcoco coroutine chain, we'd use `FromStopToken()` to convert it.

---

## Combining Everything

The real power emerges when you combine hotcoco coroutines with P2300 algorithms.

### when_all with Task Senders

Run multiple tasks concurrently using P2300's `when_all`:

```cpp
Task<int> FetchA() { co_return 10; }
Task<int> FetchB() { co_return 20; }

auto sender = stdexec::when_all(
    execution::AsSender(FetchA()),
    execution::AsSender(FetchB())
);

auto [a, b] = *stdexec::sync_wait(std::move(sender));
// a == 10, b == 20
```

### Scheduler + Pipeline

Run a sender pipeline on a specific executor:

```cpp
auto exec = LibuvExecutor::Create().Value();
auto scheduler = execution::AsScheduler(*exec);

std::thread executor_thread([&] { exec->Run(); });

auto sender = stdexec::schedule(scheduler)
            | stdexec::then([] { return 42; });

auto [value] = *stdexec::sync_wait(std::move(sender));
// value == 42, computed on the executor's thread

exec->Post([&] { exec->Stop(); });
executor_thread.join();
```

### P2300 Senders Inside Coroutines

Mix sender pipelines with coroutine control flow:

```cpp
Task<int> HybridCoroutine() {
    // Use P2300 sender for one step
    auto sender = stdexec::just(21)
                | stdexec::then([](int x) { return x * 2; });

    int from_sender = co_await execution::AsAwaitable(std::move(sender));

    // Use hotcoco coroutines for another step
    int from_task = co_await ComputeMore(from_sender);

    co_return from_task;
}
```

---

## Implementation Deep Dive

### stdexec Customization Points

stdexec uses **member-based customization** (not the older `tag_invoke` pattern). To satisfy a concept, your type declares a member typedef and implements the required methods:

```cpp
// Scheduler: declare concept tag + schedule() method
class HotcocoScheduler {
    using scheduler_concept = stdexec::scheduler_t;   // "I am a scheduler"

    auto schedule() const noexcept -> ScheduleSender;  // Required method
    friend bool operator==(/* ... */);                 // Required by concept
};

// Sender: declare concept tag + connect() + get_completion_signatures()
class ScheduleSender {
    using sender_concept = stdexec::sender_t;          // "I am a sender"

    template <class Receiver>
    auto connect(Receiver rcvr) const noexcept;        // Required method

    template <class, class...>
    static consteval auto get_completion_signatures();  // Required method
};
```

### -fno-exceptions Compatibility

hotcoco is built with `-fno-exceptions`. stdexec detects this automatically via `__EXCEPTIONS` and disables exception-dependent machinery. The adapter layer cooperates by:

- Never using `set_error(exception_ptr)` in completion signatures
- Calling `std::abort()` in `set_error` handlers (unreachable in practice)
- Using `unhandled_exception() { std::abort(); }` in bridge coroutine promises

### Architecture Overview

```
┌──────────────────────────────────────────────────────────────┐
│                    hotcoco (existing)                         │
│                                                              │
│  Task<T>         Executor        CancellationToken           │
│    │                │                    │                    │
└────┼────────────────┼────────────────────┼────────────────────┘
     │                │                    │
     ▼                ▼                    ▼
┌────────────────────────────────────────────────────────────────┐
│              execution/ adapter layer (new)                    │
│                                                                │
│  AsSender()     AsScheduler()    FromStopToken()               │
│  Task<T> →      Executor →       std::stop_token →             │
│  TaskSender     HotcocoScheduler CancellationToken             │
│                                                                │
│  AsAwaitable()                   LinkCancellation()            │
│  sender →                        CancellationToken →           │
│  co_awaitable                    std::stop_source              │
│                                                                │
└────────────────────────────────────────────────────────────────┘
     │                │                    │
     ▼                ▼                    ▼
┌──────────────────────────────────────────────────────────────┐
│               P2300 / stdexec ecosystem                      │
│                                                              │
│  stdexec::then()  stdexec::when_all()  stdexec::sync_wait()  │
│  stdexec::just()  stdexec::schedule()  std::stop_token       │
└──────────────────────────────────────────────────────────────┘
```

### File Map

| File | Purpose |
|------|---------|
| `include/hotcoco/execution/scheduler.hpp` | `Executor` → P2300 `scheduler` |
| `include/hotcoco/execution/sender.hpp` | `Task<T>` → P2300 `sender` |
| `include/hotcoco/execution/as_awaitable.hpp` | P2300 `sender` → `co_await` |
| `include/hotcoco/execution/stop_token_adapter.hpp` | `stop_token` ↔ `CancellationToken` |

---

## Summary

| Concept | hotcoco | P2300 | Adapter |
|---------|---------|-------|---------|
| Execution context | `Executor` | `scheduler` | `AsScheduler()` |
| Async operation | `Task<T>` | `sender` | `AsSender()` |
| Await a sender | `co_await task` | `connect` + `start` | `AsAwaitable()` |
| Cancellation | `CancellationToken` | `std::stop_token` | `FromStopToken()` / `LinkCancellation()` |

### Key Takeaways

1. **Zero impact on existing code** — the adapter is opt-in via `ENABLE_STDEXEC`
2. **Thin layer** — each adapter is ~100 lines, minimal maintenance burden
3. **Bidirectional** — hotcoco types work in P2300 pipelines, and P2300 senders work in hotcoco coroutines
4. **Cancellation bridged** — `stop_token` and `CancellationToken` propagate signals in both directions
5. **No exceptions needed** — all adapters work under `-fno-exceptions`

---

## Next Steps

- Explore stdexec algorithms: `let_value`, `when_all`, `transfer`, `split`
- Build custom senders for hotcoco I/O operations (TCP, timers)
- Watch P2300's standardization progress toward C++26
