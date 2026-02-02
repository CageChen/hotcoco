// ============================================================================
// Generator Unit Tests
// ============================================================================

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "hotcoco/core/generator.hpp"

using namespace hotcoco;

// ============================================================================
// Basic Generator Tests
// ============================================================================

TEST(GeneratorTest, SimpleRange) {
    auto range = [](int start, int end) -> Generator<int> {
        for (int i = start; i < end; ++i) {
            co_yield i;
        }
    };

    std::vector<int> result;
    for (int value : range(0, 5)) {
        result.push_back(value);
    }

    EXPECT_EQ(result, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST(GeneratorTest, EmptyGenerator) {
    auto empty = []() -> Generator<int> {
        co_return;  // Immediately returns, no values
    };

    std::vector<int> result;
    for (int value : empty()) {
        result.push_back(value);
    }

    EXPECT_TRUE(result.empty());
}

TEST(GeneratorTest, SingleValue) {
    auto single = []() -> Generator<int> {
        co_yield 42;
    };

    std::vector<int> result;
    for (int value : single()) {
        result.push_back(value);
    }

    EXPECT_EQ(result, (std::vector<int>{42}));
}

// ============================================================================
// Lazy Evaluation Tests
// ============================================================================

TEST(GeneratorTest, LazyEvaluation) {
    int count = 0;

    auto counted = [&]() -> Generator<int> {
        for (int i = 0; i < 10; ++i) {
            ++count;
            co_yield i;
        }
    };

    auto gen = counted();
    EXPECT_EQ(count, 0);  // Nothing computed yet

    auto it = gen.begin();
    EXPECT_EQ(count, 1);  // First value computed

    ++it;
    EXPECT_EQ(count, 2);  // Second value computed

    // Stop early - remaining values never computed
}

TEST(GeneratorTest, InfiniteGenerator) {
    auto naturals = []() -> Generator<int> {
        int n = 0;
        while (true) {
            co_yield n++;
        }
    };

    auto gen = naturals();
    auto it = gen.begin();

    // Take first 5 values from infinite sequence
    std::vector<int> first5;
    for (int i = 0; i < 5; ++i) {
        first5.push_back(*it);
        ++it;
    }

    EXPECT_EQ(first5, (std::vector<int>{0, 1, 2, 3, 4}));
}

// ============================================================================
// Different Types Tests
// ============================================================================

TEST(GeneratorTest, StringGenerator) {
    auto words = []() -> Generator<std::string> {
        co_yield "hello";
        co_yield "hotcoco";
        co_yield "world";
    };

    std::vector<std::string> result;
    for (const auto& word : words()) {
        result.push_back(word);
    }

    EXPECT_EQ(result, (std::vector<std::string>{"hello", "hotcoco", "world"}));
}

TEST(GeneratorTest, MoveOnlyType) {
    auto ptrs = []() -> Generator<std::unique_ptr<int>> {
        co_yield std::make_unique<int>(1);
        co_yield std::make_unique<int>(2);
        co_yield std::make_unique<int>(3);
    };

    std::vector<int> values;
    for (auto& ptr : ptrs()) {
        values.push_back(*ptr);
    }

    EXPECT_EQ(values, (std::vector<int>{1, 2, 3}));
}

// ============================================================================
// Composition Tests
// ============================================================================

TEST(GeneratorTest, FilterPattern) {
    auto range = [](int n) -> Generator<int> {
        for (int i = 0; i < n; ++i) {
            co_yield i;
        }
    };

    // Manual filter - only even numbers
    std::vector<int> evens;
    for (int x : range(10)) {
        if (x % 2 == 0) {
            evens.push_back(x);
        }
    }

    EXPECT_EQ(evens, (std::vector<int>{0, 2, 4, 6, 8}));
}

TEST(GeneratorTest, FibonacciSequence) {
    auto fibonacci = []() -> Generator<int> {
        int a = 0, b = 1;
        while (true) {
            co_yield a;
            int next = a + b;
            a = b;
            b = next;
        }
    };

    auto gen = fibonacci();
    auto it = gen.begin();

    std::vector<int> first8;
    for (int i = 0; i < 8; ++i) {
        first8.push_back(*it);
        ++it;
    }

    EXPECT_EQ(first8, (std::vector<int>{0, 1, 1, 2, 3, 5, 8, 13}));
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(GeneratorTest, MoveAssignment) {
    auto range = [](int n) -> Generator<int> {
        for (int i = 0; i < n; ++i) co_yield i;
    };

    Generator<int> g1 = range(3);
    Generator<int> g2 = range(5);

    // Move-assign g2 over g1
    g1 = std::move(g2);

    std::vector<int> result;
    for (int v : g1) result.push_back(v);

    EXPECT_EQ(result, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST(GeneratorTest, DestroyMidIteration) {
    int count = 0;
    {
        auto gen = [&]() -> Generator<int> {
            for (int i = 0; ; ++i) {
                count++;
                co_yield i;
            }
        };

        auto g = gen();
        auto it = g.begin();
        EXPECT_EQ(*it, 0);
        ++it;
        EXPECT_EQ(*it, 1);
        // Destroy generator mid-iteration — no crash or leak
    }
    EXPECT_EQ(count, 2);
}

TEST(GeneratorTest, LargeSequence) {
    auto range = [](int n) -> Generator<int> {
        for (int i = 0; i < n; ++i) co_yield i;
    };

    int sum = 0;
    for (int v : range(10000)) sum += v;
    EXPECT_EQ(sum, 49995000);  // sum of 0..9999
}

TEST(GeneratorTest, NestedGenerators) {
    auto inner = [](int start, int end) -> Generator<int> {
        for (int i = start; i < end; ++i) co_yield i;
    };

    auto outer = [&]() -> Generator<int> {
        for (int v : inner(0, 3)) co_yield v;
        for (int v : inner(10, 13)) co_yield v;
    };

    std::vector<int> result;
    for (int v : outer()) result.push_back(v);
    EXPECT_EQ(result, (std::vector<int>{0, 1, 2, 10, 11, 12}));
}

// ============================================================================
// Bug regression: begin() on exhausted generator must not resume done handle
// ============================================================================
// Previously, begin() called handle_.resume() without checking handle_.done(),
// which is undefined behavior on an exhausted generator.

TEST(GeneratorTest, BeginOnExhaustedGenerator) {
    auto single = []() -> Generator<int> {
        co_yield 1;
    };

    auto gen = single();

    // First iteration: exhaust the generator
    std::vector<int> result;
    for (int v : gen) result.push_back(v);
    EXPECT_EQ(result, (std::vector<int>{1}));

    // Second call to begin() on exhausted generator must return end()
    auto it = gen.begin();
    EXPECT_EQ(it, gen.end());
}
