// ============================================================================
// Cancellation Tests
// ============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "hotcoco/core/cancellation.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/sync/sync_wait.hpp"

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// Basic Token Tests
// ============================================================================

TEST(CancellationTest, DefaultTokenNotCancelled) {
    CancellationToken token;
    EXPECT_FALSE(token.IsCancelled());
    EXPECT_FALSE(token.IsValid());
}

TEST(CancellationTest, SourceCreatesValidToken) {
    CancellationSource source;
    auto token = source.GetToken();
    
    EXPECT_TRUE(token.IsValid());
    EXPECT_FALSE(token.IsCancelled());
}

TEST(CancellationTest, CancelPropagates) {
    CancellationSource source;
    auto token = source.GetToken();
    
    EXPECT_FALSE(token.IsCancelled());
    
    source.Cancel();
    
    EXPECT_TRUE(token.IsCancelled());
    EXPECT_TRUE(source.IsCancelled());
}

TEST(CancellationTest, MultipleTokens) {
    CancellationSource source;
    auto token1 = source.GetToken();
    auto token2 = source.GetToken();
    
    EXPECT_FALSE(token1.IsCancelled());
    EXPECT_FALSE(token2.IsCancelled());
    
    source.Cancel();
    
    EXPECT_TRUE(token1.IsCancelled());
    EXPECT_TRUE(token2.IsCancelled());
}

TEST(CancellationTest, BoolConversion) {
    CancellationSource source;
    auto token = source.GetToken();
    
    // Token is valid (not cancelled), so bool is true
    EXPECT_TRUE(static_cast<bool>(token));
    
    source.Cancel();
    
    // Now cancelled, so bool is false
    EXPECT_FALSE(static_cast<bool>(token));
}

// ============================================================================
// Callback Tests
// ============================================================================

TEST(CancellationTest, CallbackOnCancel) {
    CancellationSource source;
    auto token = source.GetToken();
    
    std::atomic<bool> called{false};
    token.OnCancel([&called] { called = true; });
    
    EXPECT_FALSE(called.load());
    
    source.Cancel();
    
    EXPECT_TRUE(called.load());
}

TEST(CancellationTest, CallbackIfAlreadyCancelled) {
    CancellationSource source;
    source.Cancel();
    
    auto token = source.GetToken();
    
    std::atomic<bool> called{false};
    token.OnCancel([&called] { called = true; });
    
    // Callback should be called immediately
    EXPECT_TRUE(called.load());
}

TEST(CancellationTest, MultipleCallbacks) {
    CancellationSource source;
    auto token = source.GetToken();
    
    std::atomic<int> count{0};
    token.OnCancel([&count] { count++; });
    token.OnCancel([&count] { count++; });
    token.OnCancel([&count] { count++; });
    
    source.Cancel();
    
    EXPECT_EQ(count.load(), 3);
}

TEST(CancellationTest, UnregisterCallback) {
    CancellationSource source;
    auto token = source.GetToken();
    
    std::atomic<bool> called{false};
    size_t handle = token.OnCancel([&called] { called = true; });
    
    token.Unregister(handle);
    
    source.Cancel();
    
    EXPECT_FALSE(called.load());
}

TEST(CancellationTest, CallbackGuard) {
    CancellationSource source;
    auto token = source.GetToken();
    
    std::atomic<bool> called{false};
    
    {
        CancellationCallbackGuard guard(token, [&called] { called = true; });
        // Guard goes out of scope, unregisters callback
    }
    
    source.Cancel();
    
    EXPECT_FALSE(called.load());
}

// ============================================================================
// Thread Safety Tests
// ============================================================================

TEST(CancellationTest, CancelFromAnotherThread) {
    CancellationSource source;
    auto token = source.GetToken();
    
    std::atomic<bool> saw_cancel{false};
    
    std::thread worker([&token, &saw_cancel] {
        while (!token.IsCancelled()) {
            std::this_thread::sleep_for(1ms);
        }
        saw_cancel = true;
    });
    
    std::this_thread::sleep_for(10ms);
    source.Cancel();
    
    worker.join();
    
    EXPECT_TRUE(saw_cancel.load());
}

// ============================================================================
// Coroutine Integration
// ============================================================================

Task<int> CountUntilCancelled(CancellationToken token) {
    int count = 0;
    while (!token.IsCancelled()) {
        count++;
        if (count >= 1000) break;  // Safety limit
    }
    co_return count;
}

TEST(CancellationTest, WithCoroutine) {
    CancellationSource source;
    auto token = source.GetToken();
    
    // Pre-cancel
    source.Cancel();
    
    auto result = SyncWait(CountUntilCancelled(token));
    
    // Should exit immediately with count 0
    EXPECT_EQ(result, 0);
}

TEST(CancellationTest, NoneToken) {
    auto token = CancellationToken::None();
    
    EXPECT_FALSE(token.IsValid());
    EXPECT_FALSE(token.IsCancelled());

    // Should not crash when registering on None token
    token.OnCancel([] {});
}

// ============================================================================
// Additional Edge Cases
// ============================================================================

TEST(CancellationTest, DoubleCancelIsIdempotent) {
    CancellationSource source;
    auto token = source.GetToken();

    source.Cancel();
    source.Cancel();  // Should not crash

    EXPECT_TRUE(token.IsCancelled());
}

TEST(CancellationTest, UnregisterNonExistentHandle) {
    CancellationSource source;
    auto token = source.GetToken();

    // Unregister a handle that doesn't exist
    token.Unregister(99999);  // Should not crash

    source.Cancel();
}

TEST(CancellationTest, CopyToken) {
    CancellationSource source;
    auto token1 = source.GetToken();
    auto token2 = token1;  // Copy

    EXPECT_FALSE(token1.IsCancelled());
    EXPECT_FALSE(token2.IsCancelled());

    source.Cancel();

    EXPECT_TRUE(token1.IsCancelled());
    EXPECT_TRUE(token2.IsCancelled());
}

// ============================================================================
// Bug regression: RegisterCallback must not invoke callback under mutex lock
// ============================================================================
// If the token is already cancelled when RegisterCallback is called, the
// callback was previously invoked while holding the mutex. If the callback
// itself tried to unregister or register another callback, it would deadlock.

TEST(CancellationTest, RegisterCallbackOnCancelledTokenNoDeadlock) {
    CancellationSource source;
    source.Cancel();

    auto token = source.GetToken();

    // Register a callback that itself registers another callback.
    // If RegisterCallback invokes the callback under the lock, this deadlocks.
    std::atomic<int> call_count{0};
    token.OnCancel([&] {
        call_count++;
        // This second registration also triggers immediate invocation
        // because the token is already cancelled.
        token.OnCancel([&] {
            call_count++;
        });
    });

    EXPECT_EQ(call_count.load(), 2);
}

TEST(CancellationTest, SourceDestroyed) {
    auto token = CancellationToken::None();

    {
        CancellationSource source;
        token = source.GetToken();
        EXPECT_TRUE(token.IsValid());
        EXPECT_FALSE(token.IsCancelled());
    }

    // Source is destroyed but token should still work
    // (shared_ptr keeps state alive)
    EXPECT_TRUE(token.IsValid());
}
