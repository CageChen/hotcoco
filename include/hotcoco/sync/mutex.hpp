// ============================================================================
// hotcoco/sync/mutex.hpp - Async-aware Mutex
// ============================================================================
//
// AsyncMutex provides mutual exclusion for coroutines. Unlike std::mutex,
// waiting coroutines suspend instead of blocking the thread.
//
// DESIGN PHILOSOPHY:
// ------------------
// 1. NON-BLOCKING: Waiting coroutines yield, don't block thread
// 2. FAIR ORDERING: FIFO wakeup for waiting coroutines
// 3. RAII SUPPORT: ScopedLock for exception-safe locking
//
// USAGE:
// ------
//   AsyncMutex mutex;
//   
//   Task<void> Work() {
//       auto lock = co_await mutex.Lock();
//       // Critical section
//   }  // Automatically unlocked
//
// ============================================================================

#pragma once

#include <coroutine>
#include <deque>
#include <mutex>
#include <optional>

namespace hotcoco {

class AsyncMutex {
public:
    // ========================================================================
    // ScopedLock - RAII lock guard
    // ========================================================================
    class ScopedLock {
    public:
        explicit ScopedLock(AsyncMutex& mutex) : mutex_(&mutex) {}
        
        ~ScopedLock() {
            if (mutex_) {
                mutex_->Unlock();
            }
        }
        
        // Move only
        ScopedLock(ScopedLock&& other) noexcept : mutex_(other.mutex_) {
            other.mutex_ = nullptr;
        }
        ScopedLock& operator=(ScopedLock&& other) noexcept {
            if (this != &other) {
                if (mutex_) mutex_->Unlock();
                mutex_ = other.mutex_;
                other.mutex_ = nullptr;
            }
            return *this;
        }
        
        ScopedLock(const ScopedLock&) = delete;
        ScopedLock& operator=(const ScopedLock&) = delete;
        
    private:
        AsyncMutex* mutex_;
    };
    
    // ========================================================================
    // LockAwaitable
    // ========================================================================
    class LockAwaitable {
    public:
        explicit LockAwaitable(AsyncMutex& mutex) : mutex_(mutex) {}
        
        bool await_ready() {
            std::lock_guard<std::mutex> lock(mutex_.mutex_);
            if (!mutex_.locked_) {
                mutex_.locked_ = true;
                return true;
            }
            return false;
        }
        
        bool await_suspend(std::coroutine_handle<> h) {
            std::lock_guard<std::mutex> lock(mutex_.mutex_);
            if (!mutex_.locked_) {
                mutex_.locked_ = true;
                return false;  // Don't suspend, acquired lock
            }
            mutex_.waiters_.push_back(h);
            return true;  // Suspend
        }
        
        ScopedLock await_resume() {
            return ScopedLock(mutex_);
        }
        
    private:
        AsyncMutex& mutex_;
    };
    
    // ========================================================================
    // TryLockAwaitable
    // ========================================================================
    class TryLockAwaitable {
    public:
        explicit TryLockAwaitable(AsyncMutex& mutex) : mutex_(mutex) {}
        
        bool await_ready() { return true; }  // Always ready
        void await_suspend(std::coroutine_handle<>) {}
        
        std::optional<ScopedLock> await_resume() {
            std::lock_guard<std::mutex> lock(mutex_.mutex_);
            if (!mutex_.locked_) {
                mutex_.locked_ = true;
                return ScopedLock(mutex_);
            }
            return std::nullopt;
        }
        
    private:
        AsyncMutex& mutex_;
    };
    
    // ========================================================================
    // Public API
    // ========================================================================
    
    LockAwaitable Lock() {
        return LockAwaitable(*this);
    }
    
    TryLockAwaitable TryLock() {
        return TryLockAwaitable(*this);
    }
    
private:
    void Unlock() {
        std::coroutine_handle<> to_wake;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!waiters_.empty()) {
                to_wake = waiters_.front();
                waiters_.pop_front();
                // Keep locked_ true - passing to waiter
            } else {
                locked_ = false;
            }
        }
        
        if (to_wake) {
            to_wake.resume();
        }
    }
    
    std::mutex mutex_;
    bool locked_ = false;
    std::deque<std::coroutine_handle<>> waiters_;
};

}  // namespace hotcoco
