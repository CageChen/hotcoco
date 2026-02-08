// ============================================================================
// hotcoco/sync/latch.hpp - Async-aware One-Shot Barrier
// ============================================================================
//
// AsyncLatch allows one or more coroutines to wait until a count reaches zero.
// It's a one-shot synchronization primitive - once it reaches zero, it stays
// released forever.
//
// USAGE:
// ------
//   AsyncLatch latch(3);  // Wait for 3 signals
//
//   // Workers count down
//   Spawn(executor, [&]() -> Task<void> {
//       co_await DoWork();
//       latch.CountDown();
//   }());
//
//   // Waiter
//   co_await latch.Wait();  // Waits until count reaches 0
//
// ============================================================================

#pragma once

#include <atomic>
#include <coroutine>
#include <mutex>
#include <vector>

namespace hotcoco {

class AsyncLatch {
   public:
    explicit AsyncLatch(size_t count) : count_(count) {}

    // Non-copyable
    AsyncLatch(const AsyncLatch&) = delete;
    AsyncLatch& operator=(const AsyncLatch&) = delete;

    // ========================================================================
    // WaitAwaitable
    // ========================================================================
    class WaitAwaitable {
       public:
        explicit WaitAwaitable(AsyncLatch& latch) : latch_(latch) {}

        bool await_ready() { return latch_.count_.load() == 0; }

        bool await_suspend(std::coroutine_handle<> h) {
            std::lock_guard<std::mutex> lock(latch_.mutex_);
            if (latch_.count_.load() == 0) {
                return false;  // Already released
            }
            latch_.waiters_.push_back(h);
            return true;
        }

        void await_resume() {}

       private:
        AsyncLatch& latch_;
    };

    // ========================================================================
    // Public API
    // ========================================================================

    WaitAwaitable Wait() { return WaitAwaitable(*this); }

    void CountDown(size_t n = 1) {
        std::vector<std::coroutine_handle<>> to_wake;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            size_t old = count_.load();
            size_t new_count = (old >= n) ? (old - n) : 0;
            count_.store(new_count);

            if (new_count == 0) {
                to_wake = std::move(waiters_);
            }
        }

        for (auto h : to_wake) {
            h.resume();
        }
    }

    size_t Count() const { return count_.load(); }

    bool IsReleased() const { return count_.load() == 0; }

   private:
    std::atomic<size_t> count_;
    std::mutex mutex_;
    std::vector<std::coroutine_handle<>> waiters_;
};

}  // namespace hotcoco
