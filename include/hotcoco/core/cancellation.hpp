// ============================================================================
// hotcoco/core/cancellation.hpp - Cancellation Token Support
// ============================================================================
//
// CancellationToken provides cooperative cancellation for coroutines.
// A token can be passed through a chain of coroutines, and any coroutine
// can check if cancellation has been requested.
//
// DESIGN PHILOSOPHY:
// ------------------
// 1. COOPERATIVE: Coroutines must check the token - not preemptive
// 2. THREAD-SAFE: Token can be cancelled from any thread
// 3. COMPOSABLE: Pass tokens through coroutine chains
//
// USAGE:
// ------
//   CancellationSource source;
//   auto token = source.GetToken();
//
//   auto task = LongRunningWork(token);
//
//   // Cancel after timeout
//   std::thread([&source] {
//       std::this_thread::sleep_for(5s);
//       source.Cancel();
//   }).detach();
//
// IN COROUTINE:
// -------------
//   Task<void> Worker(CancellationToken token) {
//       while (!token.IsCancelled()) {
//           co_await ProcessNextItem();
//       }
//       co_return;
//   }
//
// ============================================================================

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace hotcoco {

// Forward declaration
class CancellationSource;

// ============================================================================
// CancellationState - Shared state between source and tokens
// ============================================================================
class CancellationState {
   public:
    CancellationState() = default;

    // Non-copyable, non-movable
    CancellationState(const CancellationState&) = delete;
    CancellationState& operator=(const CancellationState&) = delete;

    bool IsCancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }

    void Cancel() {
        if (cancelled_.exchange(true, std::memory_order_acq_rel)) {
            return;  // Already cancelled
        }

        // Invoke callbacks
        std::vector<std::function<void()>> callbacks_to_call;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks_to_call = std::move(callbacks_);
        }

        for (auto& callback : callbacks_to_call) {
            callback();
        }
    }

    // Register a callback to be called when cancelled
    // Returns a handle that can be used to unregister
    size_t RegisterCallback(std::function<void()> callback) {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // If already cancelled, invoke outside the lock to avoid deadlock
            if (cancelled_.load(std::memory_order_acquire)) {
                // Fall through to invoke below
            } else {
                size_t handle = next_handle_++;
                callbacks_.push_back(std::move(callback));
                callback_handles_.push_back(handle);
                return handle;
            }
        }
        // Invoke outside lock — already cancelled
        callback();
        return 0;
    }

    void UnregisterCallback(size_t handle) {
        if (handle == 0) return;

        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < callback_handles_.size(); ++i) {
            if (callback_handles_[i] == handle) {
                callbacks_.erase(callbacks_.begin() + static_cast<long>(i));
                callback_handles_.erase(callback_handles_.begin() + static_cast<long>(i));
                break;
            }
        }
    }

   private:
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    std::vector<std::function<void()>> callbacks_;
    std::vector<size_t> callback_handles_;
    size_t next_handle_{1};
};

// ============================================================================
// CancellationToken - Read-only view of cancellation state
// ============================================================================
class CancellationToken {
   public:
    // Default token that is never cancelled
    CancellationToken() : state_(nullptr) {}

    // Check if cancellation has been requested
    bool IsCancelled() const noexcept { return state_ && state_->IsCancelled(); }

    // Implicit bool conversion
    explicit operator bool() const noexcept { return !IsCancelled(); }

    // Register callback for cancellation notification
    size_t OnCancel(std::function<void()> callback) {
        if (state_) {
            return state_->RegisterCallback(std::move(callback));
        }
        return 0;
    }

    // Unregister a previously registered callback
    void Unregister(size_t handle) {
        if (state_) {
            state_->UnregisterCallback(handle);
        }
    }

    // Check if this is a valid token (has associated state)
    bool IsValid() const noexcept { return state_ != nullptr; }

    // Create a "never cancelled" token
    [[nodiscard]] static CancellationToken None() { return CancellationToken{}; }

   private:
    friend class CancellationSource;

    explicit CancellationToken(std::shared_ptr<CancellationState> state) : state_(std::move(state)) {}

    std::shared_ptr<CancellationState> state_;
};

// ============================================================================
// CancellationSource - Creates and controls tokens
// ============================================================================
class CancellationSource {
   public:
    CancellationSource() : state_(std::make_shared<CancellationState>()) {}

    // Non-copyable but movable
    CancellationSource(const CancellationSource&) = delete;
    CancellationSource& operator=(const CancellationSource&) = delete;
    CancellationSource(CancellationSource&&) = default;
    CancellationSource& operator=(CancellationSource&&) = default;

    // Get a token that can be passed to coroutines
    [[nodiscard]] CancellationToken GetToken() const { return CancellationToken(state_); }

    // Request cancellation
    void Cancel() {
        if (state_) {
            state_->Cancel();
        }
    }

    // Check if cancellation was requested
    bool IsCancelled() const noexcept { return state_ && state_->IsCancelled(); }

   private:
    std::shared_ptr<CancellationState> state_;
};

// ============================================================================
// RAII guard for callback registration
// ============================================================================
class CancellationCallbackGuard {
   public:
    CancellationCallbackGuard(CancellationToken token, std::function<void()> callback)
        : token_(std::move(token)), handle_(token_.OnCancel(std::move(callback))) {}

    ~CancellationCallbackGuard() { token_.Unregister(handle_); }

    // Non-copyable, non-movable
    CancellationCallbackGuard(const CancellationCallbackGuard&) = delete;
    CancellationCallbackGuard& operator=(const CancellationCallbackGuard&) = delete;

   private:
    CancellationToken token_;
    size_t handle_;
};

}  // namespace hotcoco
