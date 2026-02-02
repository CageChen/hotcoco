// ============================================================================
// hotcoco/core/channel.hpp - CSP-Style Channels
// ============================================================================
//
// Channel provides Go-style communication between coroutines.
// A Channel<T> is an async-safe queue that coroutines can send to and
// receive from without blocking the event loop.
//
// DESIGN PHILOSOPHY:
// ------------------
// 1. BOUNDED BUFFER: Configurable capacity prevents unbounded memory growth
// 2. ASYNC OPERATIONS: Send/Receive are awaitable, not blocking
// 3. CLOSE SEMANTICS: Channel can be closed, receivers get std::nullopt
//
// ============================================================================

#pragma once

#include <atomic>
#include <cassert>
#include <coroutine>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace hotcoco {

template <typename T>
class Channel {
public:
    explicit Channel(size_t capacity = 1) : capacity_(capacity) {
        assert(capacity >= 1 && "Channel capacity must be at least 1");
    }
    
    ~Channel() {
        // Do NOT call Close() here — Close() resumes waiters inline, and
        // those coroutines may re-enter this Channel (e.g. another
        // Send/Receive), accessing a partially-destroyed object.
        // Instead, mark closed and clear waiters without resuming them.
        // The coroutine frames are owned by their callers and will be
        // cleaned up when those owners destroy the Task/SpawnedTask.
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        receiver_waiters_.clear();
        sender_waiters_.clear();
    }
    
    // Non-copyable
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    
    // ========================================================================
    // Send Awaitable
    // ========================================================================
    class SendAwaitable {
    public:
        SendAwaitable(Channel& ch, T value)
            : channel_(ch), value_(std::move(value)) {}

        bool await_ready() {
            std::coroutine_handle<> receiver_to_wake;
            {
                std::lock_guard<std::mutex> lock(channel_.mutex_);
                if (channel_.closed_) {
                    failed_ = true;
                    return true;
                }
                // If a receiver is waiting, hand the value directly
                // into its result_ slot — avoids buffer round-trip race.
                if (!channel_.receiver_waiters_.empty()) {
                    auto waiter = std::move(channel_.receiver_waiters_.front());
                    channel_.receiver_waiters_.erase(channel_.receiver_waiters_.begin());
                    *waiter.result = std::move(value_);
                    receiver_to_wake = waiter.handle;
                    sent_.store(true, std::memory_order_relaxed);
                } else if (channel_.buffer_.size() < channel_.capacity_) {
                    channel_.buffer_.push_back(std::move(value_));
                    sent_.store(true, std::memory_order_relaxed);
                } else {
                    return false;  // Must suspend
                }
            }
            if (receiver_to_wake) {
                receiver_to_wake.resume();
            }
            return true;
        }

        bool await_suspend(std::coroutine_handle<> h) {
            std::coroutine_handle<> receiver_to_wake;
            {
                std::lock_guard<std::mutex> lock(channel_.mutex_);
                if (channel_.closed_) {
                    failed_ = true;
                    return false;  // Don't suspend, resume immediately
                }
                // If a receiver is waiting, hand the value directly
                if (!channel_.receiver_waiters_.empty()) {
                    auto waiter = std::move(channel_.receiver_waiters_.front());
                    channel_.receiver_waiters_.erase(channel_.receiver_waiters_.begin());
                    *waiter.result = std::move(value_);
                    receiver_to_wake = waiter.handle;
                    sent_.store(true, std::memory_order_relaxed);
                } else if (channel_.buffer_.size() < channel_.capacity_) {
                    channel_.buffer_.push_back(std::move(value_));
                    sent_.store(true, std::memory_order_relaxed);
                } else {
                    channel_.sender_waiters_.push_back({h, std::move(value_), &sent_});
                    return true;  // Suspend
                }
            }
            if (receiver_to_wake) {
                receiver_to_wake.resume();
            }
            return false;  // Don't suspend
        }

        bool await_resume() {
            return sent_.load(std::memory_order_acquire) && !failed_;
        }

    private:
        Channel& channel_;
        T value_;
        std::atomic<bool> sent_{false};
        bool failed_ = false;
    };

    // ========================================================================
    // Receive Awaitable
    // ========================================================================
    class ReceiveAwaitable {
    public:
        explicit ReceiveAwaitable(Channel& ch) : channel_(ch) {}

        bool await_ready() {
            std::coroutine_handle<> sender_to_wake;
            {
                std::lock_guard<std::mutex> lock(channel_.mutex_);
                if (!channel_.buffer_.empty()) {
                    result_ = std::move(channel_.buffer_.front());
                    channel_.buffer_.pop_front();
                    // Wake a waiting sender if any (buffer space freed)
                    if (!channel_.sender_waiters_.empty()) {
                        auto waiter = std::move(channel_.sender_waiters_.front());
                        channel_.sender_waiters_.erase(channel_.sender_waiters_.begin());
                        channel_.buffer_.push_back(std::move(waiter.value));
                        if (waiter.sent_flag) waiter.sent_flag->store(true, std::memory_order_release);
                        sender_to_wake = waiter.handle;
                    }
                } else if (channel_.closed_) {
                    return true;  // Return nullopt
                } else {
                    return false;  // Must suspend
                }
            }
            if (sender_to_wake) {
                sender_to_wake.resume();
            }
            return true;
        }

        bool await_suspend(std::coroutine_handle<> h) {
            std::coroutine_handle<> sender_to_wake;
            {
                std::lock_guard<std::mutex> lock(channel_.mutex_);
                if (!channel_.buffer_.empty()) {
                    result_ = std::move(channel_.buffer_.front());
                    channel_.buffer_.pop_front();
                    // Wake a waiting sender if any
                    if (!channel_.sender_waiters_.empty()) {
                        auto waiter = std::move(channel_.sender_waiters_.front());
                        channel_.sender_waiters_.erase(channel_.sender_waiters_.begin());
                        channel_.buffer_.push_back(std::move(waiter.value));
                        if (waiter.sent_flag) waiter.sent_flag->store(true, std::memory_order_release);
                        sender_to_wake = waiter.handle;
                    }
                } else if (channel_.closed_) {
                    // Don't suspend, return nullopt
                } else {
                    // Register with a pointer to our result_ so senders
                    // can write directly, avoiding the buffer race.
                    channel_.receiver_waiters_.push_back({h, &result_});
                    return true;  // Suspend
                }
            }
            if (sender_to_wake) {
                sender_to_wake.resume();
            }
            return false;  // Don't suspend
        }

        std::optional<T> await_resume() {
            return std::move(result_);
        }

    private:
        Channel& channel_;
        std::optional<T> result_;
    };
    
    // ========================================================================
    // Public API
    // ========================================================================
    
    SendAwaitable Send(T value) {
        return SendAwaitable(*this, std::move(value));
    }
    
    ReceiveAwaitable Receive() {
        return ReceiveAwaitable(*this);
    }
    
    void Close() {
        std::vector<std::coroutine_handle<>> to_wake;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return;
            closed_ = true;

            // Collect all waiters
            for (auto& w : receiver_waiters_) {
                to_wake.push_back(w.handle);
            }
            receiver_waiters_.clear();
            for (auto& w : sender_waiters_) {
                to_wake.push_back(w.handle);
            }
            sender_waiters_.clear();
        }

        // Wake them outside lock
        for (auto h : to_wake) {
            h.resume();
        }
    }
    
    bool IsClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }
    
    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffer_.size();
    }
    
private:
    struct SenderWaiter {
        std::coroutine_handle<> handle;
        T value;
        std::atomic<bool>* sent_flag;  // Pointer to SendAwaitable::sent_
    };

    struct ReceiverWaiter {
        std::coroutine_handle<> handle;
        std::optional<T>* result;  // Direct handoff slot
    };

    mutable std::mutex mutex_;
    std::deque<T> buffer_;
    size_t capacity_;
    bool closed_ = false;

    std::vector<ReceiverWaiter> receiver_waiters_;
    std::vector<SenderWaiter> sender_waiters_;
};

}  // namespace hotcoco
