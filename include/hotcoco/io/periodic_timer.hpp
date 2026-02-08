// ============================================================================
// hotcoco/io/periodic_timer.hpp - Coroutine Periodic Timer
// ============================================================================
//
// PeriodicTimer fires at regular intervals, allowing coroutines to perform
// repeated work with precise timing.
//
// USAGE:
// ------
//   Task<void> Heartbeat() {
//       PeriodicTimer timer(1000ms);
//       while (running) {
//           co_await timer.Wait();
//           SendHeartbeat();
//       }
//   }
//
// ============================================================================

#pragma once

#include <chrono>
#include <coroutine>
#include <functional>
#include <uv.h>

namespace hotcoco {

class PeriodicTimer {
   public:
    using Duration = std::chrono::milliseconds;

    explicit PeriodicTimer(Duration interval, uv_loop_t* loop) : interval_(interval), loop_(loop) {
        timer_ = new uv_timer_t;
        uv_timer_init(loop_, timer_);
        timer_->data = this;
    }

    ~PeriodicTimer() {
        Stop();
        // Resume any coroutine suspended on Wait() so its frame is not leaked.
        // The coroutine will resume and proceed normally (await_resume returns void).
        if (waiting_) {
            auto h = waiting_;
            waiting_ = nullptr;
            h.resume();
        }
        // uv_close is required to properly release the libuv handle.
        // Heap-allocated so the close callback can safely free it.
        uv_close(reinterpret_cast<uv_handle_t*>(timer_),
                 [](uv_handle_t* h) { delete reinterpret_cast<uv_timer_t*>(h); });
    }

    // Non-copyable
    PeriodicTimer(const PeriodicTimer&) = delete;
    PeriodicTimer& operator=(const PeriodicTimer&) = delete;

    // ========================================================================
    // WaitAwaitable
    // ========================================================================
    class WaitAwaitable {
       public:
        explicit WaitAwaitable(PeriodicTimer& timer) : timer_(timer) {}

        bool await_ready() { return false; }

        void await_suspend(std::coroutine_handle<> h) {
            timer_.waiting_ = h;

            // Start timer if not already running
            if (!timer_.running_) {
                uv_timer_start(timer_.timer_, OnTimer, static_cast<uint64_t>(timer_.interval_.count()),
                               static_cast<uint64_t>(timer_.interval_.count()));
                timer_.running_ = true;
            }
        }

        void await_resume() {}

       private:
        PeriodicTimer& timer_;
    };

    WaitAwaitable Wait() { return WaitAwaitable(*this); }

    void Stop() {
        if (running_) {
            uv_timer_stop(timer_);
            running_ = false;
        }
    }

    void SetInterval(Duration interval) {
        interval_ = interval;
        if (running_) {
            uv_timer_set_repeat(timer_, static_cast<uint64_t>(interval_.count()));
        }
    }

   private:
    static void OnTimer(uv_timer_t* handle) {
        auto* self = static_cast<PeriodicTimer*>(handle->data);
        if (self->waiting_) {
            auto h = self->waiting_;
            self->waiting_ = nullptr;
            h.resume();
        }
    }

    uv_timer_t* timer_;
    Duration interval_;
    uv_loop_t* loop_;
    bool running_ = false;
    std::coroutine_handle<> waiting_;
};

}  // namespace hotcoco
