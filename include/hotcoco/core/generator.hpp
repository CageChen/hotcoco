// ============================================================================
// hotcoco/core/generator.hpp - Lazy Sequence Generator
// ============================================================================
//
// Generator<T> is a coroutine type that produces a sequence of values lazily
// using co_yield. It's perfect for creating iterators, processing streams,
// or any situation where you want to generate values on-demand.
//
// KEY CONCEPTS:
// -------------
// 1. LAZY GENERATION: Values are only produced when requested via iteration.
//    No computation happens until you call begin() and iterate.
//
// 2. CO_YIELD: The generator uses co_yield to produce values. Each co_yield
//    suspends the coroutine and makes the value available to the caller.
//
// 3. RANGE-FOR COMPATIBLE: Generator provides begin()/end() iterators so you
//    can use it with range-based for loops.
//
// 4. SINGLE-PASS: Generators are input iterators - you can only iterate once.
//    After iteration, the generator is exhausted.
//
// USAGE:
// ------
//   Generator<int> Range(int start, int end) {
//       for (int i = start; i < end; ++i) {
//           co_yield i;
//       }
//   }
//
//   // Using range-for
//   for (int value : Range(0, 5)) {
//       std::cout << value << " ";  // 0 1 2 3 4
//   }
//
//   // Lazy evaluation - only computes what's needed
//   Generator<int> gen = Range(0, 1000000);
//   auto it = gen.begin();
//   std::cout << *it << std::endl;  // Only computed first value
//
// ============================================================================

#pragma once

#include <coroutine>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <utility>

namespace hotcoco {

// Forward declarations
template <typename T>
class Generator;

// ============================================================================
// Generator Promise Type
// ============================================================================
//
// The promise manages the generator's state, including the currently yielded
// value and any exception that occurred.
//
template <typename T>
class GeneratorPromise {
   public:
    using value_type = std::remove_reference_t<T>;
    using reference = value_type&;
    using pointer = value_type*;

    // ========================================================================
    // CUSTOMIZATION POINT: get_return_object()
    // ========================================================================
    Generator<T> get_return_object() noexcept;

    // ========================================================================
    // CUSTOMIZATION POINT: initial_suspend()
    // ========================================================================
    // We suspend immediately (lazy) - no work until iteration begins.
    //
    std::suspend_always initial_suspend() noexcept { return {}; }

    // ========================================================================
    // CUSTOMIZATION POINT: final_suspend()
    // ========================================================================
    // We must suspend at the end so the iterator can detect completion.
    // If we didn't suspend, the coroutine frame would be destroyed before
    // the iterator could check if we're done.
    //
    std::suspend_always final_suspend() noexcept { return {}; }

    // ========================================================================
    // CUSTOMIZATION POINT: yield_value()
    // ========================================================================
    // Called when the coroutine executes: co_yield value;
    // We store a pointer to the value (avoiding copies) and suspend.
    //
    // The stored pointer is valid because the yielded value lives in the
    // coroutine frame, which exists until we resume or destroy.
    //
    std::suspend_always yield_value(value_type& value) noexcept {
        current_value_ = std::addressof(value);
        return {};
    }

    // For rvalue yields, we store in our own storage
    std::suspend_always yield_value(value_type&& value) noexcept {
        owned_value_ = std::move(value);
        current_value_ = std::addressof(owned_value_.value());
        return {};
    }

    // ========================================================================
    // CUSTOMIZATION POINT: return_void()
    // ========================================================================
    // Generators don't return values, they just end.
    //
    void return_void() noexcept {}

    // ========================================================================
    // CUSTOMIZATION POINT: unhandled_exception()
    // ========================================================================
    // Required by the standard but should never be reached under
    // -fno-exceptions. If somehow called, abort immediately.
    //
    void unhandled_exception() noexcept { std::abort(); }

    // ========================================================================
    // Value Access
    // ========================================================================
    reference GetValue() const noexcept { return *current_value_; }

    bool HasValue() const noexcept { return current_value_ != nullptr; }

   private:
    pointer current_value_ = nullptr;
    std::optional<value_type> owned_value_;  // For rvalue yields
};

// ============================================================================
// Generator Iterator
// ============================================================================
//
// The iterator is what makes Generator usable with range-for loops.
// It resumes the coroutine on each ++ and checks for completion.
//
template <typename T>
class GeneratorIterator {
   public:
    // Iterator traits for STL compatibility
    using iterator_category = std::input_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = typename GeneratorPromise<T>::value_type;
    using reference = typename GeneratorPromise<T>::reference;
    using pointer = typename GeneratorPromise<T>::pointer;

    using Handle = std::coroutine_handle<GeneratorPromise<T>>;

    // ========================================================================
    // Construction
    // ========================================================================
    GeneratorIterator() noexcept = default;  // end() iterator

    explicit GeneratorIterator(Handle handle) noexcept : handle_(handle) {}

    // ========================================================================
    // Iterator Operations
    // ========================================================================

    // Pre-increment: Resume coroutine to get next value
    GeneratorIterator& operator++() {
        handle_.resume();
        if (handle_.done()) {
            handle_ = nullptr;  // Mark as end
        }
        return *this;
    }

    // Post-increment (required by input_iterator)
    void operator++(int) { ++(*this); }

    // Dereference: Get the current yielded value
    reference operator*() const { return handle_.promise().GetValue(); }

    pointer operator->() const { return std::addressof(operator*()); }

    // Comparison: Only compare with end sentinel
    bool operator==(const GeneratorIterator& other) const noexcept { return handle_ == other.handle_; }

    bool operator!=(const GeneratorIterator& other) const noexcept { return !(*this == other); }

   private:
    Handle handle_ = nullptr;
};

// ============================================================================
// Generator<T> - The Coroutine Return Type
// ============================================================================
//
// Generator is the type returned by generator coroutines. It provides
// begin()/end() for range-for compatibility and manages the coroutine lifetime.
//
template <typename T>
class Generator {
   public:
    using promise_type = GeneratorPromise<T>;
    using Handle = std::coroutine_handle<promise_type>;
    using iterator = GeneratorIterator<T>;

    // ========================================================================
    // Construction / Destruction
    // ========================================================================

    explicit Generator(Handle handle) noexcept : handle_(handle) {}

    ~Generator() {
        if (handle_) {
            handle_.destroy();
        }
    }

    // Move-only semantics
    Generator(const Generator&) = delete;
    Generator& operator=(const Generator&) = delete;

    Generator(Generator&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    Generator& operator=(Generator&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    // ========================================================================
    // Range Interface
    // ========================================================================

    // begin(): Start iteration by resuming to get first value
    iterator begin() {
        if (handle_ && !handle_.done()) {
            handle_.resume();
            if (handle_.done()) {
                return end();
            }
        } else {
            return end();
        }
        return iterator{handle_};
    }

    // end(): Sentinel for end of sequence
    iterator end() noexcept { return iterator{}; }

   private:
    Handle handle_;
};

// ============================================================================
// Promise::get_return_object() Implementation
// ============================================================================
template <typename T>
Generator<T> GeneratorPromise<T>::get_return_object() noexcept {
    return Generator<T>{Generator<T>::Handle::from_promise(*this)};
}

}  // namespace hotcoco
