// ============================================================================
// Example 01: Basic Task Usage
// ============================================================================
//
// This example demonstrates the fundamental Task<T> coroutine type.
//
// RUN:
//   cd build && ./examples/01_basic_task
//
// ============================================================================

#include "hotcoco/hotcoco.hpp"

#include <iostream>

using namespace hotcoco;

// A simple coroutine that returns a value
Task<int> ComputeAnswer() {
    std::cout << "Computing the answer..." << std::endl;
    co_return 42;
}

// A coroutine that awaits another coroutine
Task<int> DoubleTheAnswer() {
    std::cout << "Getting the answer first..." << std::endl;
    int answer = co_await ComputeAnswer();
    std::cout << "Doubling it..." << std::endl;
    co_return answer * 2;
}

// A void coroutine
Task<void> PrintMessage(const std::string& msg) {
    std::cout << "Message: " << msg << std::endl;
    co_return;
}

int main() {
    std::cout << "=== Hotcoco Example 01: Basic Task ===" << std::endl;
    std::cout << std::endl;

    // Example 1: Simple task
    std::cout << "--- Example 1: Simple Task ---" << std::endl;
    int result = SyncWait(ComputeAnswer());
    std::cout << "Result: " << result << std::endl;
    std::cout << std::endl;

    // Example 2: Chained tasks
    std::cout << "--- Example 2: Chained Tasks ---" << std::endl;
    int doubled = SyncWait(DoubleTheAnswer());
    std::cout << "Doubled result: " << doubled << std::endl;
    std::cout << std::endl;

    // Example 3: Void task
    std::cout << "--- Example 3: Void Task ---" << std::endl;
    SyncWait(PrintMessage("Hello from hotcoco!"));
    std::cout << std::endl;

    // Example 4: Lambda coroutine
    std::cout << "--- Example 4: Lambda Coroutine ---" << std::endl;
    auto lambda_task = []() -> Task<std::string> { co_return "Lambda coroutines work too!"; };
    std::string msg = SyncWait(lambda_task());
    std::cout << msg << std::endl;

    std::cout << std::endl;
    std::cout << "=== Done! ===" << std::endl;
    return 0;
}
