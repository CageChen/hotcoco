// ============================================================================
// hotcoco/execution/scheduler.hpp - Executor → P2300 Scheduler Adapter
// ============================================================================
//
// Wraps any hotcoco::Executor as a P2300 scheduler, enabling interop with
// the std::execution (P2300) sender/receiver ecosystem.
//
// The adapter is thin: schedule() posts work to the executor via Post(),
// and completes by calling set_value on the receiver.
//
// USAGE:
// ------
//   auto executor = LibuvExecutor::Create().Value();
//   auto scheduler = hotcoco::execution::AsScheduler(*executor);
//
//   auto sender = stdexec::schedule(scheduler)
//               | stdexec::then([]{ return 42; });
//
// ============================================================================

#pragma once

#include "hotcoco/io/executor.hpp"

#include <functional>
#include <stdexec/execution.hpp>
#include <utility>

namespace hotcoco::execution {

class HotcocoScheduler;

// ============================================================================
// ScheduleOperation - operation_state for schedule() sender
// ============================================================================
template <typename Receiver>
class ScheduleOperation {
   public:
    ScheduleOperation(Executor* executor, Receiver rcvr) noexcept : executor_(executor), receiver_(std::move(rcvr)) {}

    ScheduleOperation(ScheduleOperation&&) = delete;
    ScheduleOperation& operator=(ScheduleOperation&&) = delete;

    void start() noexcept {
        executor_->Post([this] {
            if constexpr (stdexec::unstoppable_token<stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>>) {
                stdexec::set_value(std::move(receiver_));
            } else if (stdexec::get_stop_token(stdexec::get_env(receiver_)).stop_requested()) {
                stdexec::set_stopped(std::move(receiver_));
            } else {
                stdexec::set_value(std::move(receiver_));
            }
        });
    }

   private:
    Executor* executor_;
    Receiver receiver_;
};

// ============================================================================
// ScheduleSender - sender returned by schedule()
// ============================================================================
class ScheduleSender {
   public:
    using sender_concept = stdexec::sender_t;

    explicit ScheduleSender(Executor* executor) noexcept : executor_(executor) {}

    template <class Receiver>
    auto connect(Receiver rcvr) const noexcept -> ScheduleOperation<Receiver> {
        return ScheduleOperation<Receiver>{executor_, std::move(rcvr)};
    }

    template <class, class...>
    static consteval auto get_completion_signatures() noexcept {
        return stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_stopped_t()>{};
    }

   private:
    friend class HotcocoScheduler;
    Executor* executor_;
};

// ============================================================================
// HotcocoScheduler - P2300 scheduler wrapping a hotcoco Executor
// ============================================================================
class HotcocoScheduler {
   public:
    using scheduler_concept = stdexec::scheduler_t;

    explicit HotcocoScheduler(Executor* executor) noexcept : executor_(executor) {}

    [[nodiscard]] auto schedule() const noexcept -> ScheduleSender { return ScheduleSender{executor_}; }

    friend bool operator==(const HotcocoScheduler& a, const HotcocoScheduler& b) noexcept {
        return a.executor_ == b.executor_;
    }

   private:
    Executor* executor_;
};

// Factory function
inline HotcocoScheduler AsScheduler(Executor& exec) noexcept {
    return HotcocoScheduler{&exec};
}

}  // namespace hotcoco::execution
