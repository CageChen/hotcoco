// ============================================================================
// Example 07: Bridging hotcoco with P2300 std::execution
// ============================================================================
//
// This example demonstrates the adapter layer that connects hotcoco
// coroutines with the P2300 Senders/Receivers ecosystem via stdexec.
//
// Build with:
//   cmake -B build -G Ninja -DENABLE_STDEXEC=ON && ninja -C build
//
// RUN:
//   cd build && ./examples/07_stdexec_bridge
//
// ============================================================================

#ifdef HOTCOCO_HAS_STDEXEC

#include "hotcoco/hotcoco.hpp"

#include <iostream>
#include <stdexec/execution.hpp>
#include <thread>

using namespace hotcoco;

// ============================================================================
// 1. Task<T> as a P2300 sender
// ============================================================================

Task<int> ComputeAsync() {
    std::cout << "  [ComputeAsync] producing 42" << std::endl;
    co_return 42;
}

void demo_task_as_sender() {
    std::cout << "--- Demo 1: Task<T> as P2300 sender ---" << std::endl;

    // Wrap a hotcoco Task as a sender and feed it into a stdexec pipeline
    auto sender = execution::AsSender(ComputeAsync()) | stdexec::then([](int x) {
                      std::cout << "  [then] doubling " << x << std::endl;
                      return x * 2;
                  });

    auto result = stdexec::sync_wait(std::move(sender));
    auto [value] = *result;
    std::cout << "  Result: " << value << std::endl;
    std::cout << std::endl;
}

// ============================================================================
// 2. Scheduler adapter — run work on a hotcoco executor
// ============================================================================

void demo_scheduler() {
    std::cout << "--- Demo 2: Executor as P2300 scheduler ---" << std::endl;

    auto exec = LibuvExecutor::Create().Value();
    auto scheduler = execution::AsScheduler(*exec);

    // Run the executor in a background thread
    std::thread executor_thread([&] { exec->Run(); });

    // Build a P2300 pipeline that runs on the hotcoco executor
    auto sender = stdexec::schedule(scheduler) | stdexec::then([] {
                      std::cout << "  [schedule+then] running on executor thread " << std::this_thread::get_id()
                                << std::endl;
                      return 99;
                  });

    auto result = stdexec::sync_wait(std::move(sender));
    auto [value] = *result;
    std::cout << "  Result: " << value << std::endl;

    exec->Post([&] { exec->Stop(); });
    executor_thread.join();
    std::cout << std::endl;
}

// ============================================================================
// 3. co_await a P2300 sender inside a hotcoco coroutine
// ============================================================================

Task<int> HybridCoroutine() {
    std::cout << "  [HybridCoroutine] building sender pipeline..." << std::endl;

    // Build a P2300 sender pipeline
    auto sender = stdexec::just(21) | stdexec::then([](int x) {
                      std::cout << "  [sender pipeline] " << x << " * 2" << std::endl;
                      return x * 2;
                  });

    // co_await it inside a hotcoco coroutine!
    int from_sender = co_await execution::AsAwaitable(std::move(sender));

    std::cout << "  [HybridCoroutine] got " << from_sender << " from sender" << std::endl;
    co_return from_sender;
}

void demo_sender_as_awaitable() {
    std::cout << "--- Demo 3: co_await a P2300 sender ---" << std::endl;

    int result = SyncWait(HybridCoroutine());
    std::cout << "  Result: " << result << std::endl;
    std::cout << std::endl;
}

// ============================================================================
// 4. when_all — concurrent tasks via P2300 composition
// ============================================================================

Task<int> FetchA() {
    std::cout << "  [FetchA] producing 10" << std::endl;
    co_return 10;
}

Task<int> FetchB() {
    std::cout << "  [FetchB] producing 20" << std::endl;
    co_return 20;
}

void demo_when_all() {
    std::cout << "--- Demo 4: when_all with Task senders ---" << std::endl;

    auto sender = stdexec::when_all(execution::AsSender(FetchA()), execution::AsSender(FetchB()));

    auto result = stdexec::sync_wait(std::move(sender));
    auto [a, b] = *result;
    std::cout << "  Results: a=" << a << ", b=" << b << ", sum=" << (a + b) << std::endl;
    std::cout << std::endl;
}

// ============================================================================
// 5. Cancellation bridge — stop_token ↔ CancellationToken
// ============================================================================

void demo_cancellation_bridge() {
    std::cout << "--- Demo 5: Cancellation bridge ---" << std::endl;

    // Direction 1: std::stop_token → CancellationToken
    {
        std::cout << "  [stop_token → CancellationToken]" << std::endl;
        std::stop_source ss;
        auto [token, bridge] = execution::FromStopToken(ss.get_token());

        std::cout << "    Before stop: cancelled=" << token.IsCancelled() << std::endl;
        ss.request_stop();
        std::cout << "    After stop:  cancelled=" << token.IsCancelled() << std::endl;
    }

    // Direction 2: CancellationToken → std::stop_source
    {
        std::cout << "  [CancellationToken → stop_source]" << std::endl;
        CancellationSource source;
        std::stop_source ss;

        {
            auto link = execution::LinkCancellation(source.GetToken(), ss);
            std::cout << "    Before cancel: stop_requested=" << ss.stop_requested() << std::endl;
            source.Cancel();
            std::cout << "    After cancel:  stop_requested=" << ss.stop_requested() << std::endl;
        }
        // link destroyed — callback unregistered
    }

    std::cout << std::endl;
}

// ============================================================================
// 6. Full pipeline — scheduler + async task + sender composition
// ============================================================================

void demo_full_pipeline() {
    std::cout << "--- Demo 6: Full pipeline ---" << std::endl;

    auto exec = LibuvExecutor::Create().Value();
    auto scheduler = execution::AsScheduler(*exec);

    std::thread executor_thread([&] { exec->Run(); });

    // Build a pipeline: schedule on executor → co_await a sender inside a
    // coroutine → transform with stdexec::then
    auto make_task = [&scheduler]() -> Task<int> {
        // co_await a sender pipeline inside a hotcoco coroutine
        auto sender = stdexec::schedule(scheduler) | stdexec::then([] { return 7; });
        int base = co_await execution::AsAwaitable(std::move(sender));
        co_return base * 6;  // 42
    };

    auto sender = execution::AsSender(make_task()) | stdexec::then([](int x) {
                      std::cout << "  [final then] got " << x << std::endl;
                      return x;
                  });

    auto result = stdexec::sync_wait(std::move(sender));
    auto [value] = *result;
    std::cout << "  Result: " << value << std::endl;

    exec->Post([&] { exec->Stop(); });
    executor_thread.join();
    std::cout << std::endl;
}

int main() {
    std::cout << "=== Hotcoco Example 07: P2300 std::execution Bridge ===" << std::endl;
    std::cout << "(main thread: " << std::this_thread::get_id() << ")" << std::endl;
    std::cout << std::endl;

    demo_task_as_sender();
    demo_scheduler();
    demo_sender_as_awaitable();
    demo_when_all();
    demo_cancellation_bridge();
    demo_full_pipeline();

    std::cout << "=== Done! ===" << std::endl;
    return 0;
}

#else

#include <iostream>

int main() {
    std::cout << "This example requires ENABLE_STDEXEC=ON." << std::endl;
    std::cout << "Rebuild with: cmake -B build -DENABLE_STDEXEC=ON" << std::endl;
    return 1;
}

#endif
