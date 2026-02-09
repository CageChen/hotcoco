// ============================================================================
// hotcoco/core/task_group.hpp - Structured Concurrency
// ============================================================================
//
// TaskGroup is a dynamic task container that allows adding tasks at any time
// and co_await-ing completion of all tasks. This enables structured concurrency
// patterns where all spawned work is guaranteed to complete before the scope
// exits.
//
// USAGE:
// ------
//   TaskGroup group(executor);
//   group.Start(DoWorkA());
//   group.Start(DoWorkB());
//   group.Start(DoWorkC());
//   co_await group;  // Wait for all tasks to complete
//
// ============================================================================

#pragma once

#include "hotcoco/core/detached_task.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/io/executor.hpp"
#include "hotcoco/sync/event.hpp"

#include <atomic>
#include <cassert>
#include <functional>
#include <memory>

namespace hotcoco {

class TaskGroup {
   public:
    explicit TaskGroup(Executor& executor) : executor_(executor), shared_(std::make_shared<SharedState>()) {
        // Start with event signaled so that co_await on an empty group
        // passes through immediately instead of hanging forever.
        shared_->event.Set();
    }

    // Non-copyable, non-movable
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
    TaskGroup(TaskGroup&&) = delete;
    TaskGroup& operator=(TaskGroup&&) = delete;

    ~TaskGroup() {
        // In debug builds, assert all tasks completed
        assert(shared_->size.load(std::memory_order_acquire) == 0 &&
               "TaskGroup destroyed with outstanding tasks. Did you forget co_await?");
    }

    // ========================================================================
    // Start - Add a task to the group
    // ========================================================================
    void Start(Task<void> task) {
        // Reset the event so we can await it again
        shared_->event.Reset();
        shared_->size.fetch_add(1, std::memory_order_release);

        auto detached = MakeDetached(std::move(task));
        auto shared = shared_;  // capture shared_ptr, not this
        detached.SetCallback([shared]() {
            if (shared->size.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                shared->event.Set();
            }
        });

        // Start() resumes the coroutine inline (cooperative execution).
        // The DetachedTask self-destructs when the coroutine completes.
        detached.Start();
    }

    // ========================================================================
    // Awaitable interface - co_await group waits for all tasks
    // ========================================================================
    [[nodiscard]] auto operator co_await() { return shared_->event.Wait(); }

    // ========================================================================
    // Query
    // ========================================================================
    size_t Size() const { return shared_->size.load(std::memory_order_acquire); }

    bool Empty() const { return Size() == 0; }

   private:
    struct SharedState {
        std::atomic<uint64_t> size{0};
        AsyncEvent event;
    };

    [[maybe_unused]] Executor& executor_;
    std::shared_ptr<SharedState> shared_;
};

}  // namespace hotcoco
