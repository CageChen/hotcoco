// ============================================================================
// hotcoco/sync/semaphore.hpp - Async-aware Counting Semaphore
// ============================================================================
//
// AsyncSemaphore provides bounded concurrency control for coroutines.
// Unlike std::counting_semaphore, waiting coroutines suspend instead of
// blocking the thread.
//
// USAGE:
// ------
//   AsyncSemaphore sem(3);  // Allow 3 concurrent operations
//
//   Task<void> Work() {
//       auto guard = co_await sem.Acquire();
//       // At most 3 tasks here at once
//   }  // Automatically released
//
// ============================================================================

#pragma once

#include <coroutine>
#include <deque>
#include <mutex>

namespace hotcoco {

class AsyncSemaphore {
   public:
    // ========================================================================
    // Guard - RAII release
    // ========================================================================
    class Guard {
       public:
        explicit Guard(AsyncSemaphore& sem) : sem_(&sem) {}

        ~Guard() {
            if (sem_) {
                sem_->Release();
            }
        }

        // Move only
        Guard(Guard&& other) noexcept : sem_(other.sem_) { other.sem_ = nullptr; }
        Guard& operator=(Guard&& other) noexcept {
            if (this != &other) {
                if (sem_) sem_->Release();
                sem_ = other.sem_;
                other.sem_ = nullptr;
            }
            return *this;
        }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

       private:
        AsyncSemaphore* sem_;
    };

    // ========================================================================
    // AcquireAwaitable
    // ========================================================================
    class AcquireAwaitable {
       public:
        explicit AcquireAwaitable(AsyncSemaphore& sem) : sem_(sem) {}

        bool await_ready() {
            std::lock_guard<std::mutex> lock(sem_.mutex_);
            if (sem_.count_ > 0) {
                sem_.count_--;
                return true;
            }
            return false;
        }

        bool await_suspend(std::coroutine_handle<> h) {
            std::lock_guard<std::mutex> lock(sem_.mutex_);
            if (sem_.count_ > 0) {
                sem_.count_--;
                return false;  // Don't suspend
            }
            sem_.waiters_.push_back(h);
            return true;
        }

        Guard await_resume() { return Guard(sem_); }

       private:
        AsyncSemaphore& sem_;
    };

    // ========================================================================
    // Public API
    // ========================================================================

    explicit AsyncSemaphore(size_t initial_count) : count_(initial_count) {}

    AcquireAwaitable Acquire() { return AcquireAwaitable(*this); }

    void Release() {
        std::coroutine_handle<> to_wake;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!waiters_.empty()) {
                to_wake = waiters_.front();
                waiters_.pop_front();
            } else {
                count_++;
            }
        }

        if (to_wake) {
            to_wake.resume();
        }
    }

    size_t Available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

   private:
    mutable std::mutex mutex_;
    size_t count_;
    std::deque<std::coroutine_handle<>> waiters_;
};

}  // namespace hotcoco
