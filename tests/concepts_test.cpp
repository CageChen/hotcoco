// ============================================================================
// Concepts Tests
// ============================================================================

#include <gtest/gtest.h>

#include <coroutine>
#include <string>

#include "hotcoco/core/concepts.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/core/generator.hpp"
#include "hotcoco/io/timer.hpp"
#include "hotcoco/sync/event.hpp"

using namespace hotcoco;

// ============================================================================
// Test Awaiter types
// ============================================================================

// Minimal awaiter returning void with void suspend
struct VoidAwaiter {
    bool await_ready() { return true; }
    void await_suspend(std::coroutine_handle<>) {}
    void await_resume() {}
};

// Awaiter returning bool from await_suspend
struct BoolSuspendAwaiter {
    bool await_ready() { return false; }
    bool await_suspend(std::coroutine_handle<>) { return true; }
    int await_resume() { return 42; }
};

// Awaiter returning coroutine_handle from await_suspend (symmetric transfer)
struct HandleSuspendAwaiter {
    bool await_ready() { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<>) {
        return std::noop_coroutine();
    }
    std::string await_resume() { return "hello"; }
};

// Not an awaiter — missing await_ready
struct NotAnAwaiter1 {
    void await_suspend(std::coroutine_handle<>) {}
    void await_resume() {}
};

// Not an awaiter — missing await_suspend
struct NotAnAwaiter2 {
    bool await_ready() { return true; }
    void await_resume() {}
};

// Not an awaiter — missing await_resume
struct NotAnAwaiter3 {
    bool await_ready() { return true; }
    void await_suspend(std::coroutine_handle<>) {}
};

// Not an awaiter — await_ready returns wrong type
struct NotAnAwaiter4 {
    int await_ready() { return 0; }  // not bool-convertible? Actually int IS convertible to bool
    void await_suspend(std::coroutine_handle<>) {}
    void await_resume() {}
};

// Plain type, not an awaiter at all
struct PlainType {
    int value = 0;
};

// ============================================================================
// Test Awaitable types (with operator co_await)
// ============================================================================

// Type with member operator co_await
struct MemberCoAwait {
    VoidAwaiter operator co_await() { return {}; }
};

// Type with member operator co_await returning int-producing awaiter
struct MemberCoAwaitInt {
    BoolSuspendAwaiter operator co_await() { return {}; }
};

// ============================================================================
// Awaiter concept tests
// ============================================================================

TEST(ConceptsTest, AwaiterWithVoidSuspend) {
    static_assert(Awaiter<VoidAwaiter>);
}

TEST(ConceptsTest, AwaiterWithBoolSuspend) {
    static_assert(Awaiter<BoolSuspendAwaiter>);
}

TEST(ConceptsTest, AwaiterWithHandleSuspend) {
    static_assert(Awaiter<HandleSuspendAwaiter>);
}

TEST(ConceptsTest, NotAwaiterMissingReady) {
    static_assert(!Awaiter<NotAnAwaiter1>);
}

TEST(ConceptsTest, NotAwaiterMissingSuspend) {
    static_assert(!Awaiter<NotAnAwaiter2>);
}

TEST(ConceptsTest, NotAwaiterMissingResume) {
    static_assert(!Awaiter<NotAnAwaiter3>);
}

TEST(ConceptsTest, IntConvertsToBoolSoIsAwaiter) {
    // int is convertible to bool, so this IS a valid awaiter
    static_assert(Awaiter<NotAnAwaiter4>);
}

TEST(ConceptsTest, PlainTypeIsNotAwaiter) {
    static_assert(!Awaiter<PlainType>);
}

TEST(ConceptsTest, IntIsNotAwaiter) {
    static_assert(!Awaiter<int>);
}

// ============================================================================
// Awaitable concept tests
// ============================================================================

TEST(ConceptsTest, AwaiterIsAwaitable) {
    // Any Awaiter is also Awaitable
    static_assert(Awaitable<VoidAwaiter>);
    static_assert(Awaitable<BoolSuspendAwaiter>);
    static_assert(Awaitable<HandleSuspendAwaiter>);
}

TEST(ConceptsTest, MemberCoAwaitIsAwaitable) {
    static_assert(Awaitable<MemberCoAwait>);
}

TEST(ConceptsTest, MemberCoAwaitIntIsAwaitable) {
    static_assert(Awaitable<MemberCoAwaitInt>);
}

TEST(ConceptsTest, PlainTypeIsNotAwaitable) {
    static_assert(!Awaitable<PlainType>);
    static_assert(!Awaitable<int>);
    static_assert(!Awaitable<std::string>);
}

// ============================================================================
// Real hotcoco types
// ============================================================================

TEST(ConceptsTest, TaskIsAwaitable) {
    static_assert(Awaitable<Task<int>>);
    static_assert(Awaitable<Task<void>>);
    static_assert(Awaitable<Task<std::string>>);
}

TEST(ConceptsTest, AsyncEventWaitIsAwaiter) {
    // AsyncEvent::WaitAwaitable is returned by Wait()
    AsyncEvent event;
    auto awaitable = event.Wait();
    static_assert(Awaiter<decltype(awaitable)>);
    static_assert(Awaitable<decltype(awaitable)>);
}

// ============================================================================
// AwaitResult trait tests
// ============================================================================

TEST(ConceptsTest, AwaitResultVoid) {
    static_assert(std::is_void_v<AwaitResult<VoidAwaiter>>);
}

TEST(ConceptsTest, AwaitResultInt) {
    static_assert(std::is_same_v<AwaitResult<BoolSuspendAwaiter>, int>);
}

TEST(ConceptsTest, AwaitResultString) {
    static_assert(std::is_same_v<AwaitResult<HandleSuspendAwaiter>, std::string>);
}

TEST(ConceptsTest, AwaitResultTaskInt) {
    static_assert(std::is_same_v<AwaitResult<Task<int>>, int>);
}

TEST(ConceptsTest, AwaitResultTaskVoid) {
    static_assert(std::is_void_v<AwaitResult<Task<void>>>);
}

TEST(ConceptsTest, AwaitResultMemberCoAwaitInt) {
    static_assert(std::is_same_v<AwaitResult<MemberCoAwaitInt>, int>);
}

// ============================================================================
// AwaitableOf concept tests
// ============================================================================

TEST(ConceptsTest, AwaitableOfMatches) {
    static_assert(AwaitableOf<BoolSuspendAwaiter, int>);
    static_assert(AwaitableOf<HandleSuspendAwaiter, std::string>);
    static_assert(AwaitableOf<Task<int>, int>);
}

TEST(ConceptsTest, AwaitableOfMismatch) {
    // BoolSuspendAwaiter returns int, not string
    static_assert(!AwaitableOf<BoolSuspendAwaiter, std::string>);
    // Task<int> returns int, not double (int IS convertible to double though)
    static_assert(AwaitableOf<Task<int>, double>);  // int -> double is implicit
    // Task<int> is not convertible to string
    static_assert(!AwaitableOf<Task<int>, std::string>);
}

TEST(ConceptsTest, AwaitableOfVoidConcept) {
    static_assert(AwaitableOfVoid<VoidAwaiter>);
    static_assert(AwaitableOfVoid<Task<void>>);
    static_assert(!AwaitableOfVoid<Task<int>>);
    static_assert(!AwaitableOfVoid<BoolSuspendAwaiter>);
}

// ============================================================================
// AwaitableOf through operator co_await
// ============================================================================

TEST(ConceptsTest, AwaitableOfThroughCoAwait) {
    static_assert(AwaitableOf<MemberCoAwaitInt, int>);
    static_assert(!AwaitableOf<MemberCoAwaitInt, std::string>);
}

// ============================================================================
// Additional Type Tests
// ============================================================================

TEST(ConceptsTest, GeneratorIsNotAwaitable) {
    static_assert(!Awaitable<Generator<int>>);
}

TEST(ConceptsTest, AsyncSleepIsAwaitable) {
    static_assert(Awaitable<AsyncSleep>);
    static_assert(AwaitableOfVoid<AsyncSleep>);
}
