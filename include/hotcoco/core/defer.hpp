// ============================================================================
// hotcoco/core/defer.hpp - Deferred Cleanup
// ============================================================================
//
// Defer provides Go-style deferred execution for cleanup actions.
// The deferred function runs when the Defer object goes out of scope.
//
// USAGE:
// ------
//   Task<void> Work() {
//       auto file = OpenFile();
//       Defer cleanup([&] { file.Close(); });
//       
//       co_await ProcessFile(file);
//       // file.Close() runs automatically here
//   }
//
// ============================================================================

#pragma once

#include <functional>
#include <utility>

namespace hotcoco {

// ============================================================================
// Defer - RAII Deferred Execution
// ============================================================================
class Defer {
public:
    template <typename F>
    explicit Defer(F&& func) : cleanup_(std::forward<F>(func)) {}
    
    ~Defer() {
        if (cleanup_) {
            cleanup_();
        }
    }
    
    // Non-copyable
    Defer(const Defer&) = delete;
    Defer& operator=(const Defer&) = delete;
    
    // Move only
    Defer(Defer&& other) noexcept : cleanup_(std::move(other.cleanup_)) {
        other.cleanup_ = nullptr;
    }
    Defer& operator=(Defer&& other) noexcept {
        if (this != &other) {
            if (cleanup_) cleanup_();
            cleanup_ = std::move(other.cleanup_);
            other.cleanup_ = nullptr;
        }
        return *this;
    }
    
    // Cancel the deferred action
    void Cancel() {
        cleanup_ = nullptr;
    }
    
    // Execute now and cancel
    void ExecuteNow() {
        if (cleanup_) {
            cleanup_();
            cleanup_ = nullptr;
        }
    }
    
private:
    std::function<void()> cleanup_;
};

// ============================================================================
// DEFER Macro - Convenient syntax
// ============================================================================
// Usage:
//   DEFER([&] { cleanup_code; });
//
// Creates a uniquely-named Defer object that runs cleanup_code on scope exit.

#define HOTCOCO_DEFER_CONCAT_IMPL(a, b) a##b
#define HOTCOCO_DEFER_CONCAT(a, b) HOTCOCO_DEFER_CONCAT_IMPL(a, b)
#define DEFER(lambda) \
    ::hotcoco::Defer HOTCOCO_DEFER_CONCAT(_hotcoco_defer_, __LINE__){lambda}

}  // namespace hotcoco
