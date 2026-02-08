// ============================================================================
// hotcoco/sync/rwlock.hpp - Async-aware Read-Write Lock
// ============================================================================
//
// AsyncRWLock provides multiple-reader/single-writer locking for coroutines.
// Waiting coroutines suspend instead of blocking threads.
//
// USAGE:
// ------
//   AsyncRWLock lock;
//
//   Task<void> Reader() {
//       auto guard = co_await lock.ReadLock();
//       // Multiple readers allowed
//   }
//
//   Task<void> Writer() {
//       auto guard = co_await lock.WriteLock();
//       // Exclusive access
//   }
//
// ============================================================================

#pragma once

#include <coroutine>
#include <deque>
#include <mutex>
#include <vector>

namespace hotcoco {

class AsyncRWLock {
   public:
    // ========================================================================
    // ReadGuard - RAII read lock release
    // ========================================================================
    class ReadGuard {
       public:
        explicit ReadGuard(AsyncRWLock& lock) : lock_(&lock) {}

        ~ReadGuard() {
            if (lock_) {
                lock_->ReadUnlock();
            }
        }

        ReadGuard(ReadGuard&& other) noexcept : lock_(other.lock_) { other.lock_ = nullptr; }
        ReadGuard& operator=(ReadGuard&& other) noexcept {
            if (this != &other) {
                if (lock_) lock_->ReadUnlock();
                lock_ = other.lock_;
                other.lock_ = nullptr;
            }
            return *this;
        }

        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;

       private:
        AsyncRWLock* lock_;
    };

    // ========================================================================
    // WriteGuard - RAII write lock release
    // ========================================================================
    class WriteGuard {
       public:
        explicit WriteGuard(AsyncRWLock& lock) : lock_(&lock) {}

        ~WriteGuard() {
            if (lock_) {
                lock_->WriteUnlock();
            }
        }

        WriteGuard(WriteGuard&& other) noexcept : lock_(other.lock_) { other.lock_ = nullptr; }
        WriteGuard& operator=(WriteGuard&& other) noexcept {
            if (this != &other) {
                if (lock_) lock_->WriteUnlock();
                lock_ = other.lock_;
                other.lock_ = nullptr;
            }
            return *this;
        }

        WriteGuard(const WriteGuard&) = delete;
        WriteGuard& operator=(const WriteGuard&) = delete;

       private:
        AsyncRWLock* lock_;
    };

    // ========================================================================
    // ReadLockAwaitable
    // ========================================================================
    class ReadLockAwaitable {
       public:
        explicit ReadLockAwaitable(AsyncRWLock& lock) : lock_(lock) {}

        bool await_ready() {
            std::lock_guard<std::mutex> guard(lock_.mutex_);
            // Can read if no writer and no waiting writers (to prevent starvation)
            if (!lock_.write_locked_ && lock_.write_waiters_.empty()) {
                lock_.reader_count_++;
                return true;
            }
            return false;
        }

        bool await_suspend(std::coroutine_handle<> h) {
            std::lock_guard<std::mutex> guard(lock_.mutex_);
            if (!lock_.write_locked_ && lock_.write_waiters_.empty()) {
                lock_.reader_count_++;
                return false;
            }
            lock_.read_waiters_.push_back(h);
            return true;
        }

        ReadGuard await_resume() { return ReadGuard(lock_); }

       private:
        AsyncRWLock& lock_;
    };

    // ========================================================================
    // WriteLockAwaitable
    // ========================================================================
    class WriteLockAwaitable {
       public:
        explicit WriteLockAwaitable(AsyncRWLock& lock) : lock_(lock) {}

        bool await_ready() {
            std::lock_guard<std::mutex> guard(lock_.mutex_);
            if (!lock_.write_locked_ && lock_.reader_count_ == 0) {
                lock_.write_locked_ = true;
                return true;
            }
            return false;
        }

        bool await_suspend(std::coroutine_handle<> h) {
            std::lock_guard<std::mutex> guard(lock_.mutex_);
            if (!lock_.write_locked_ && lock_.reader_count_ == 0) {
                lock_.write_locked_ = true;
                return false;
            }
            lock_.write_waiters_.push_back(h);
            return true;
        }

        WriteGuard await_resume() { return WriteGuard(lock_); }

       private:
        AsyncRWLock& lock_;
    };

    // ========================================================================
    // Public API
    // ========================================================================

    ReadLockAwaitable ReadLock() { return ReadLockAwaitable(*this); }

    WriteLockAwaitable WriteLock() { return WriteLockAwaitable(*this); }

   private:
    void ReadUnlock() {
        std::vector<std::coroutine_handle<>> to_wake;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            reader_count_--;

            // If no more readers and writer waiting, wake writer
            if (reader_count_ == 0 && !write_waiters_.empty()) {
                write_locked_ = true;
                to_wake.push_back(write_waiters_.front());
                write_waiters_.pop_front();
            }
        }

        for (auto h : to_wake) {
            h.resume();
        }
    }

    void WriteUnlock() {
        std::vector<std::coroutine_handle<>> to_wake;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            write_locked_ = false;

            // Fair policy: if readers are waiting, wake them all first
            // to prevent writer-preferring starvation. Writers and readers
            // alternate in FIFO order relative to each other.
            if (!read_waiters_.empty()) {
                // Wake all waiting readers
                reader_count_ = read_waiters_.size();
                for (auto h : read_waiters_) {
                    to_wake.push_back(h);
                }
                read_waiters_.clear();
            } else if (!write_waiters_.empty()) {
                write_locked_ = true;
                to_wake.push_back(write_waiters_.front());
                write_waiters_.pop_front();
            }
        }

        for (auto h : to_wake) {
            h.resume();
        }
    }

    std::mutex mutex_;
    size_t reader_count_ = 0;
    bool write_locked_ = false;
    std::deque<std::coroutine_handle<>> read_waiters_;
    std::deque<std::coroutine_handle<>> write_waiters_;
};

}  // namespace hotcoco
