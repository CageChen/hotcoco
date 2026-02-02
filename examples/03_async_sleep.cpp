// ============================================================================
// Example 03: Async Sleep with Event Loop
// ============================================================================
//
// This example demonstrates the LibuvExecutor and AsyncSleep for
// non-blocking delays in coroutines.
//
// RUN:
//   cd build && ./examples/03_async_sleep
//
// ============================================================================

#include <chrono>
#include <iostream>

#include "hotcoco/core/task.hpp"
#include "hotcoco/io/libuv_executor.hpp"
#include "hotcoco/io/timer.hpp"

using namespace hotcoco;
using namespace std::chrono_literals;

// Global executor pointer for stopping from coroutines
LibuvExecutor* g_executor = nullptr;

// A task that sleeps and prints
Task<void> SleepAndPrint(const char* name, std::chrono::milliseconds delay) {
    auto start = std::chrono::steady_clock::now();
    std::cout << "[" << name << "] Starting, will sleep for "
              << delay.count() << "ms..." << std::endl;

    co_await AsyncSleep(delay);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    std::cout << "[" << name << "] Woke up after "
              << elapsed.count() << "ms!" << std::endl;
}

// Main async task
Task<void> MainTask() {
    std::cout << "=== Starting async tasks ===" << std::endl;
    auto start = std::chrono::steady_clock::now();

    // Start three concurrent tasks with different delays
    // Note: In this simple example, we just schedule them and let them run
    // The tasks will interleave based on their sleep times

    auto task1 = SleepAndPrint("Task1", 100ms);
    auto task2 = SleepAndPrint("Task2", 50ms);
    auto task3 = SleepAndPrint("Task3", 150ms);

    // Schedule all three tasks
    g_executor->Schedule(task1.GetHandle());
    g_executor->Schedule(task2.GetHandle());
    g_executor->Schedule(task3.GetHandle());

    // Wait longer than all tasks to ensure they complete
    co_await AsyncSleep(200ms);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    std::cout << "=== All tasks completed in " << elapsed.count()
              << "ms ===" << std::endl;

    // Stop the executor
    g_executor->Stop();
}

// Sequential sleep demonstration
Task<void> SequentialDemo() {
    std::cout << "\n--- Sequential Sleep Demo ---" << std::endl;
    auto start = std::chrono::steady_clock::now();

    for (int i = 1; i <= 3; ++i) {
        std::cout << "Step " << i << " starting..." << std::endl;
        co_await AsyncSleep(50ms);
        std::cout << "Step " << i << " done!" << std::endl;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    std::cout << "Sequential demo took " << elapsed.count() << "ms" << std::endl;
    std::cout << "(Expected ~150ms for 3 x 50ms sleeps)" << std::endl;

    g_executor->Stop();
}

int main() {
    std::cout << "=== Hotcoco Example 03: Async Sleep ===" << std::endl;
    std::cout << std::endl;

    // Create the executor (libuv event loop)
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;
    g_executor = &executor;

    // Example 1: Concurrent tasks
    {
        std::cout << "--- Example 1: Concurrent Tasks ---" << std::endl;
        auto main_task = MainTask();
        executor.Schedule(main_task.GetHandle());
        executor.Run();
    }

    std::cout << std::endl;

    // Example 2: Sequential sleeps
    {
        auto seq_task = SequentialDemo();
        executor.Schedule(seq_task.GetHandle());
        executor.Run();
    }

    std::cout << "\n=== Done! ===" << std::endl;
    return 0;
}
