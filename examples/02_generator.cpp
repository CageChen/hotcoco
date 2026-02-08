// ============================================================================
// Example 02: Generator Usage
// ============================================================================
//
// This example demonstrates the Generator<T> coroutine type for lazy sequences.
//
// RUN:
//   cd build && ./examples/02_generator
//
// ============================================================================

#include "hotcoco/hotcoco.hpp"

#include <iostream>

using namespace hotcoco;

// A simple range generator
Generator<int> Range(int start, int end) {
    std::cout << "  [Range: starting from " << start << "]" << std::endl;
    for (int i = start; i < end; ++i) {
        std::cout << "  [Range: yielding " << i << "]" << std::endl;
        co_yield i;
    }
    std::cout << "  [Range: done]" << std::endl;
}

// Infinite Fibonacci sequence
Generator<long long> Fibonacci() {
    long long a = 0, b = 1;
    while (true) {
        co_yield a;
        long long next = a + b;
        a = b;
        b = next;
    }
}

// Generate squares of numbers
Generator<int> Squares(int n) {
    for (int i = 1; i <= n; ++i) {
        co_yield i* i;
    }
}

int main() {
    std::cout << "=== Hotcoco Example 02: Generator ===" << std::endl;
    std::cout << std::endl;

    // Example 1: Basic range with logging
    std::cout << "--- Example 1: Lazy Range (notice when yields happen) ---" << std::endl;
    std::cout << "Creating generator..." << std::endl;
    auto gen = Range(1, 4);
    std::cout << "Generator created (nothing computed yet!)" << std::endl;
    std::cout << "Starting iteration:" << std::endl;
    for (int x : gen) {
        std::cout << "Got value: " << x << std::endl;
    }
    std::cout << std::endl;

    // Example 2: Fibonacci sequence (take first N)
    std::cout << "--- Example 2: First 15 Fibonacci numbers ---" << std::endl;
    auto fib = Fibonacci();
    auto it = fib.begin();
    for (int i = 0; i < 15; ++i) {
        std::cout << *it;
        ++it;
        if (i < 14) std::cout << ", ";
    }
    std::cout << std::endl << std::endl;

    // Example 3: Transform pattern (squares)
    std::cout << "--- Example 3: First 10 squares ---" << std::endl;
    for (int sq : Squares(10)) {
        std::cout << sq << " ";
    }
    std::cout << std::endl << std::endl;

    // Example 4: Filter pattern (manual)
    std::cout << "--- Example 4: Even numbers from 0-20 ---" << std::endl;
    for (int x : Range(0, 20)) {
        if (x % 2 == 0) {
            std::cout << x << " ";
        }
    }
    std::cout << std::endl << std::endl;

    std::cout << "=== Done! ===" << std::endl;
    return 0;
}
