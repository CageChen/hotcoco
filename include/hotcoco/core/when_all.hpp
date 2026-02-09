// ============================================================================
// hotcoco/core/when_all.hpp - Concurrent Fork/Join
// ============================================================================
//
// WhenAll runs multiple tasks concurrently and waits for ALL to complete.
// Returns a tuple (variadic) or vector (homogeneous) of all results.
//
// KEY CONCEPTS:
// -------------
// 1. ATOMIC COUNTDOWN LATCH: A shared atomic counter initialized to N+1.
//    Each child decrements on completion; the last one resumes the parent.
//    The "+1" prevents a race where all children complete before the parent
//    suspends.
//
// 2. WHEN_ALL_TASK: A specialized coroutine wrapper. Its final_suspend
//    decrements the latch. Results are stored in the task's promise and
//    accessed after all tasks complete.
//
// 3. COOPERATIVE CONCURRENCY: All tasks start on the calling thread and
//    interleave at their suspension points. True OS-thread parallelism
//    occurs only if tasks internally schedule onto a thread pool.
//
// USAGE:
// ------
//   auto [a, b, c] = co_await WhenAll(TaskA(), TaskB(), TaskC());
//   auto results = co_await WhenAll(std::move(task_vector));
//
// ============================================================================

#pragma once

#include "hotcoco/core/task.hpp"

#include <atomic>
#include <coroutine>
#include <cstdlib>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace hotcoco {

namespace detail {

// ============================================================================
// WhenAllLatch - Atomic countdown for fork/join synchronization
// ============================================================================
//
// Initialized to count+1. Each child calls Notify() (fetch_sub 1) on
// completion. The parent calls TryAwait() which stores its handle and
// decrements the "+1". The last decrement (to 0) resumes the parent.
//
class WhenAllLatch {
   public:
    explicit WhenAllLatch(size_t count) noexcept : count_(count + 1) {}

    bool TryAwait(std::coroutine_handle<> awaiting) noexcept {
        awaiting_ = awaiting;
        // Consume the "+1". If count is now > 0, children are still running.
        return count_.fetch_sub(1, std::memory_order_acq_rel) > 1;
    }

    void Notify() noexcept {
        if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // We were the last one — resume the parent
            awaiting_.resume();
        }
    }

   private:
    std::atomic<size_t> count_;
    std::coroutine_handle<> awaiting_{nullptr};
};

// ============================================================================
// WhenAllTask<T> - Coroutine wrapper for each child in WhenAll
// ============================================================================
template <typename T>
class WhenAllTask;

template <typename T>
class WhenAllTaskPromise {
   public:
    WhenAllTask<T> get_return_object() noexcept;

    std::suspend_always initial_suspend() noexcept { return {}; }

    auto final_suspend() noexcept {
        struct CompletionNotifier {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<WhenAllTaskPromise> h) noexcept { h.promise().latch_->Notify(); }
            void await_resume() noexcept {}
        };
        return CompletionNotifier{};
    }

    void unhandled_exception() noexcept { std::abort(); }

    void return_value(T value) noexcept { result_ = std::move(value); }

    T& Result() & noexcept { return result_.value(); }
    T&& Result() && noexcept { return std::move(result_.value()); }

    void SetLatch(WhenAllLatch* latch) noexcept { latch_ = latch; }

   private:
    WhenAllLatch* latch_ = nullptr;
    std::optional<T> result_;
};

template <>
class WhenAllTaskPromise<void> {
   public:
    WhenAllTask<void> get_return_object() noexcept;

    std::suspend_always initial_suspend() noexcept { return {}; }

    auto final_suspend() noexcept {
        struct CompletionNotifier {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<WhenAllTaskPromise> h) noexcept { h.promise().latch_->Notify(); }
            void await_resume() noexcept {}
        };
        return CompletionNotifier{};
    }

    void unhandled_exception() noexcept { std::abort(); }

    void return_void() noexcept {}

    void Result() noexcept {}

    void SetLatch(WhenAllLatch* latch) noexcept { latch_ = latch; }

   private:
    WhenAllLatch* latch_ = nullptr;
};

template <typename T>
class WhenAllTask {
   public:
    using promise_type = WhenAllTaskPromise<T>;
    using Handle = std::coroutine_handle<promise_type>;

    explicit WhenAllTask(Handle h) noexcept : handle_(h) {}

    ~WhenAllTask() {
        if (handle_) {
            handle_.destroy();
        }
    }

    WhenAllTask(WhenAllTask&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    WhenAllTask& operator=(WhenAllTask&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    WhenAllTask(const WhenAllTask&) = delete;
    WhenAllTask& operator=(const WhenAllTask&) = delete;

    void Start(WhenAllLatch& latch) {
        handle_.promise().SetLatch(&latch);
        handle_.resume();
    }

    decltype(auto) Result() & { return handle_.promise().Result(); }

    decltype(auto) Result() && { return std::move(handle_.promise()).Result(); }

   private:
    Handle handle_;
};

template <typename T>
WhenAllTask<T> WhenAllTaskPromise<T>::get_return_object() noexcept {
    return WhenAllTask<T>{WhenAllTask<T>::Handle::from_promise(*this)};
}

inline WhenAllTask<void> WhenAllTaskPromise<void>::get_return_object() noexcept {
    return WhenAllTask<void>{WhenAllTask<void>::Handle::from_promise(*this)};
}

// Helper to wrap a Task<T> into a WhenAllTask<T>
template <typename T>
WhenAllTask<T> MakeWhenAllTask(Task<T> task) {
    co_return co_await std::move(task);
}

inline WhenAllTask<void> MakeWhenAllTask(Task<void> task) {
    co_await std::move(task);
}

// ============================================================================
// WhenAllAwaitable - Variadic (tuple) version
// ============================================================================
template <typename... Ts>
class WhenAllTupleAwaitable {
   public:
    explicit WhenAllTupleAwaitable(Task<Ts>... tasks)
        : tasks_(MakeWhenAllTask(std::move(tasks))...), latch_(sizeof...(Ts)) {}

    bool await_ready() noexcept { return sizeof...(Ts) == 0; }

    bool await_suspend(std::coroutine_handle<> awaiting) noexcept {
        StartAll(std::index_sequence_for<Ts...>{});
        return latch_.TryAwait(awaiting);
    }

    std::tuple<Ts...> await_resume() { return GetResults(std::index_sequence_for<Ts...>{}); }

   private:
    template <size_t... Is>
    void StartAll(std::index_sequence<Is...>) {
        (std::get<Is>(tasks_).Start(latch_), ...);
    }

    template <size_t... Is>
    std::tuple<Ts...> GetResults(std::index_sequence<Is...>) {
        return std::tuple<Ts...>{std::move(std::get<Is>(tasks_)).Result()...};
    }

    std::tuple<WhenAllTask<Ts>...> tasks_;
    WhenAllLatch latch_;
};

// ============================================================================
// WhenAllAwaitable - Vector version
// ============================================================================
template <typename T>
class WhenAllVectorAwaitable {
   public:
    explicit WhenAllVectorAwaitable(std::vector<Task<T>> tasks) : latch_(tasks.size()) {
        tasks_.reserve(tasks.size());
        for (auto& t : tasks) {
            tasks_.push_back(MakeWhenAllTask(std::move(t)));
        }
    }

    bool await_ready() noexcept { return tasks_.empty(); }

    bool await_suspend(std::coroutine_handle<> awaiting) noexcept {
        for (auto& t : tasks_) {
            t.Start(latch_);
        }
        return latch_.TryAwait(awaiting);
    }

    std::vector<T> await_resume() {
        std::vector<T> results;
        results.reserve(tasks_.size());
        for (auto& t : tasks_) {
            results.push_back(std::move(t).Result());
        }
        return results;
    }

   private:
    std::vector<WhenAllTask<T>> tasks_;
    WhenAllLatch latch_;
};

// Specialization for void tasks (vector)
template <>
class WhenAllVectorAwaitable<void> {
   public:
    explicit WhenAllVectorAwaitable(std::vector<Task<void>> tasks) : latch_(tasks.size()) {
        tasks_.reserve(tasks.size());
        for (auto& t : tasks) {
            tasks_.push_back(MakeWhenAllTask(std::move(t)));
        }
    }

    bool await_ready() noexcept { return tasks_.empty(); }

    bool await_suspend(std::coroutine_handle<> awaiting) noexcept {
        for (auto& t : tasks_) {
            t.Start(latch_);
        }
        return latch_.TryAwait(awaiting);
    }

    void await_resume() {
        for (auto& t : tasks_) {
            t.Result();  // Rethrows if any task threw
        }
    }

   private:
    std::vector<WhenAllTask<void>> tasks_;
    WhenAllLatch latch_;
};

}  // namespace detail

// ============================================================================
// WhenAll - Public API
// ============================================================================

// Variadic version: WhenAll(Task<A>, Task<B>, Task<C>) -> Task<tuple<A,B,C>>
template <typename... Ts>
[[nodiscard]] Task<std::tuple<Ts...>> WhenAll(Task<Ts>... tasks) {
    co_return co_await detail::WhenAllTupleAwaitable<Ts...>(std::move(tasks)...);
}

// Vector version: WhenAll(vector<Task<T>>) -> Task<vector<T>>
template <typename T>
[[nodiscard]] Task<std::vector<T>> WhenAll(std::vector<Task<T>> tasks) {
    co_return co_await detail::WhenAllVectorAwaitable<T>(std::move(tasks));
}

// Vector void version: WhenAll(vector<Task<void>>) -> Task<void>
[[nodiscard]] inline Task<void> WhenAll(std::vector<Task<void>> tasks) {
    co_await detail::WhenAllVectorAwaitable<void>(std::move(tasks));
}

}  // namespace hotcoco
