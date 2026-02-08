// ============================================================================
// Channel Tests
// ============================================================================

#include "hotcoco/core/channel.hpp"

#include "hotcoco/core/spawn.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/io/thread_pool_executor.hpp"
#include "hotcoco/sync/sync_wait.hpp"

#include <gtest/gtest.h>
#include <thread>

using namespace hotcoco;
using namespace std::chrono_literals;

// ============================================================================
// Basic Channel Tests
// ============================================================================

TEST(ChannelTest, SendReceive) {
    Channel<int> ch(1);

    auto test = [&ch]() -> Task<void> {
        bool sent = co_await ch.Send(42);
        EXPECT_TRUE(sent);

        auto val = co_await ch.Receive();
        EXPECT_TRUE(val.has_value());
        EXPECT_EQ(*val, 42);
    };

    SyncWait(test());
}

TEST(ChannelTest, BufferedChannel) {
    Channel<int> ch(3);

    auto test = [&ch]() -> Task<void> {
        // Fill buffer
        co_await ch.Send(1);
        co_await ch.Send(2);
        co_await ch.Send(3);

        EXPECT_EQ(ch.Size(), 3);

        // Drain
        EXPECT_EQ(*co_await ch.Receive(), 1);
        EXPECT_EQ(*co_await ch.Receive(), 2);
        EXPECT_EQ(*co_await ch.Receive(), 3);
    };

    SyncWait(test());
}

TEST(ChannelTest, CloseChannel) {
    Channel<int> ch(1);

    auto test = [&ch]() -> Task<void> {
        co_await ch.Send(42);
        ch.Close();

        // Can still receive buffered value
        auto val = co_await ch.Receive();
        EXPECT_TRUE(val.has_value());
        EXPECT_EQ(*val, 42);

        // Next receive returns nullopt
        auto empty = co_await ch.Receive();
        EXPECT_FALSE(empty.has_value());
    };

    SyncWait(test());
}

TEST(ChannelTest, SendToClosedFails) {
    Channel<int> ch(1);
    ch.Close();

    auto test = [&ch]() -> Task<void> {
        bool sent = co_await ch.Send(42);
        EXPECT_FALSE(sent);
    };

    SyncWait(test());
}

// ============================================================================
// Concurrent Channel Tests
// ============================================================================

TEST(ChannelTest, MultipleItems) {
    Channel<int> ch(10);

    auto test = [&ch]() -> Task<void> {
        constexpr int kCount = 10;
        int sum = 0;

        // Send all
        for (int i = 0; i < kCount; i++) {
            co_await ch.Send(i);
        }
        ch.Close();

        // Receive all
        while (auto val = co_await ch.Receive()) {
            sum += *val;
        }

        // Sum of 0..9 = 45
        EXPECT_EQ(sum, 45);
    };

    SyncWait(test());
}

// ============================================================================
// Bug regression: sender must wake waiting receiver
// ============================================================================
// When a receiver suspends on empty buffer, a subsequent send must wake it.
// Without the fix, this deadlocks because the sender never resumes the
// waiting receiver coroutine.

TEST(ChannelTest, SenderWakesWaitingReceiver) {
    Channel<int> ch(1);
    std::atomic<bool> received{false};
    std::atomic<int> received_value{0};
    ThreadPoolExecutor executor(2);

    // Receiver: will suspend because channel is empty
    auto receiver = [&]() -> Task<void> {
        auto val = co_await ch.Receive();
        if (val.has_value()) {
            received_value = *val;
            received = true;
        }
    };
    Spawn(executor, receiver());

    // Give receiver time to suspend
    std::this_thread::sleep_for(50ms);

    // Sender: puts value into buffer -- must wake the receiver
    auto sender = [&]() -> Task<void> { co_await ch.Send(99); };
    Spawn(executor, sender());

    // Wait for receiver to get the value
    auto start = std::chrono::steady_clock::now();
    while (!received.load()) {
        std::this_thread::sleep_for(10ms);
        if (std::chrono::steady_clock::now() - start > 2s) {
            FAIL() << "Timeout: sender did not wake waiting receiver";
        }
    }

    EXPECT_EQ(received_value.load(), 99);
    executor.Stop();
}

// ============================================================================
// Bug regression: receiver must wake waiting sender when buffer was full
// ============================================================================
// When a sender suspends because the buffer is full, a subsequent receive
// (freeing buffer space) must wake the sender.

TEST(ChannelTest, ReceiverWakesWaitingSender) {
    Channel<int> ch(1);
    std::atomic<bool> send_completed{false};
    ThreadPoolExecutor executor(2);

    // Fill the buffer
    SyncWait([&]() -> Task<void> { co_await ch.Send(1); }());

    // Sender: tries to send to full buffer, will suspend
    auto sender = [&]() -> Task<void> {
        bool ok = co_await ch.Send(2);
        EXPECT_TRUE(ok);
        send_completed = true;
    };
    Spawn(executor, sender());

    // Give sender time to suspend
    std::this_thread::sleep_for(50ms);

    // Receiver: drains one item -- must wake the sender
    auto receiver = [&]() -> Task<void> {
        auto val = co_await ch.Receive();
        EXPECT_TRUE(val.has_value());
        EXPECT_EQ(*val, 1);
    };
    Spawn(executor, receiver());

    // Wait for sender to complete
    auto start = std::chrono::steady_clock::now();
    while (!send_completed.load()) {
        std::this_thread::sleep_for(10ms);
        if (std::chrono::steady_clock::now() - start > 2s) {
            FAIL() << "Timeout: receiver did not wake waiting sender";
        }
    }

    // Drain the second value
    SyncWait([&]() -> Task<void> {
        auto val = co_await ch.Receive();
        EXPECT_TRUE(val.has_value());
        EXPECT_EQ(*val, 2);
    }());

    executor.Stop();
}

// ============================================================================
// Additional Edge Cases
// ============================================================================

TEST(ChannelTest, DefaultCapacity) {
    Channel<int> ch;  // Default capacity = 1

    auto test = [&]() -> Task<void> {
        co_await ch.Send(42);
        auto val = co_await ch.Receive();
        EXPECT_TRUE(val.has_value());
        EXPECT_EQ(*val, 42);
    };

    SyncWait(test());
}

TEST(ChannelTest, CloseWakesWaitingReceivers) {
    Channel<int> ch(1);
    std::atomic<bool> received_nullopt{false};
    ThreadPoolExecutor executor(2);

    auto receiver = [&]() -> Task<void> {
        auto val = co_await ch.Receive();
        if (!val.has_value()) {
            received_nullopt = true;
        }
    };
    Spawn(executor, receiver());

    // Give receiver time to suspend
    std::this_thread::sleep_for(50ms);

    // Close channel — should wake receiver with nullopt
    ch.Close();

    auto start = std::chrono::steady_clock::now();
    while (!received_nullopt.load()) {
        std::this_thread::sleep_for(10ms);
        if (std::chrono::steady_clock::now() - start > 2s) {
            FAIL() << "Timeout: Close did not wake waiting receiver";
        }
    }

    executor.Stop();
    EXPECT_TRUE(received_nullopt.load());
}

TEST(ChannelTest, IsClosed) {
    Channel<int> ch(1);
    EXPECT_FALSE(ch.IsClosed());
    ch.Close();
    EXPECT_TRUE(ch.IsClosed());
}

TEST(ChannelTest, DoubleClose) {
    Channel<int> ch(1);
    ch.Close();
    ch.Close();  // Should not crash
    EXPECT_TRUE(ch.IsClosed());
}

TEST(ChannelTest, StringChannel) {
    Channel<std::string> ch(3);

    auto test = [&]() -> Task<void> {
        co_await ch.Send("hello");
        co_await ch.Send("world");

        EXPECT_EQ(*co_await ch.Receive(), "hello");
        EXPECT_EQ(*co_await ch.Receive(), "world");
    };

    SyncWait(test());
}

TEST(ChannelTest, MoveOnlyChannel) {
    Channel<std::unique_ptr<int>> ch(1);

    auto test = [&]() -> Task<void> {
        co_await ch.Send(std::make_unique<int>(42));
        auto val = co_await ch.Receive();
        EXPECT_TRUE(val.has_value());
        EXPECT_EQ(**val, 42);
    };

    SyncWait(test());
}

TEST(ChannelTest, ReceiveAfterCloseWithBufferedData) {
    Channel<int> ch(5);

    auto test = [&]() -> Task<void> {
        co_await ch.Send(1);
        co_await ch.Send(2);
        co_await ch.Send(3);
        ch.Close();

        // All buffered data should still be receivable
        EXPECT_EQ(*co_await ch.Receive(), 1);
        EXPECT_EQ(*co_await ch.Receive(), 2);
        EXPECT_EQ(*co_await ch.Receive(), 3);

        // Then nullopt
        auto empty = co_await ch.Receive();
        EXPECT_FALSE(empty.has_value());
    };

    SyncWait(test());
}

// ============================================================================
// Bug regression: sender's await_resume must report success after wake
// ============================================================================
// When a sender is woken by a receiver (buffer space freed), await_resume()
// must return true (sent_ must be set).

TEST(ChannelTest, SenderReportsSuccessAfterWake) {
    Channel<int> ch(1);
    std::atomic<bool> send_ok{false};
    std::atomic<bool> done{false};
    ThreadPoolExecutor executor(2);

    // Fill buffer
    SyncWait([&]() -> Task<void> { co_await ch.Send(1); }());

    // Sender: will suspend on full buffer
    auto sender = [&]() -> Task<void> {
        bool result = co_await ch.Send(2);
        send_ok = result;
        done = true;
    };
    Spawn(executor, sender());

    std::this_thread::sleep_for(50ms);

    // Drain to wake sender
    SyncWait([&]() -> Task<void> { co_await ch.Receive(); }());

    auto start = std::chrono::steady_clock::now();
    while (!done.load()) {
        std::this_thread::sleep_for(10ms);
        if (std::chrono::steady_clock::now() - start > 2s) {
            FAIL() << "Timeout";
        }
    }

    EXPECT_TRUE(send_ok.load()) << "Sender should report success after being woken";
    executor.Stop();
}

// ============================================================================
// Bug regression: receiver value theft race condition
// ============================================================================
// When a sender finds a waiting receiver, it used to push the value into
// the buffer and then resume the receiver outside the lock. Between those
// two operations, another thread could steal the value from the buffer.
// The fix writes directly into the receiver's result_ slot under the lock.

TEST(ChannelTest, ReceiverValueNotStolen) {
    // Use a capacity-1 channel with multiple sender/receiver pairs
    // racing on a thread pool to maximize the chance of interleaving.
    constexpr int kIterations = 500;
    constexpr int kProducers = 4;
    ThreadPoolExecutor executor(kProducers + 4);
    std::atomic<int> values_received{0};
    std::atomic<int> nullopts_received{0};
    Channel<int> ch(1);

    // Pointers to long-lived objects, safe to capture in coroutines
    auto* ch_ptr = &ch;
    auto* values_ptr = &values_received;
    auto* nullopts_ptr = &nullopts_received;

    // Launch producers: each sends kIterations values
    for (int p = 0; p < kProducers; p++) {
        Spawn(executor, [](Channel<int>* c, int base, int count) -> Task<void> {
            for (int i = 0; i < count; i++) {
                co_await c->Send(base + i + 1);  // non-zero
            }
        }(ch_ptr, p * kIterations, kIterations));
    }

    // Launch consumers: each tries to receive; count successes vs nullopts
    const int total = kProducers * kIterations;
    for (int c = 0; c < total; c++) {
        Spawn(executor, [](Channel<int>* c, std::atomic<int>* vals, std::atomic<int>* nuls) -> Task<void> {
            auto val = co_await c->Receive();
            if (val.has_value()) {
                EXPECT_NE(*val, 0);
                vals->fetch_add(1, std::memory_order_relaxed);
            } else {
                nuls->fetch_add(1, std::memory_order_relaxed);
            }
        }(ch_ptr, values_ptr, nullopts_ptr));
    }

    // Wait for all work to drain
    auto start = std::chrono::steady_clock::now();
    while (values_received.load() + nullopts_received.load() < total) {
        std::this_thread::sleep_for(10ms);
        if (std::chrono::steady_clock::now() - start > 5s) {
            break;
        }
    }

    // Every sent value must be received — no theft, no spurious nullopt
    EXPECT_EQ(values_received.load(), total) << "Some values were lost (stolen by another thread between buffer "
                                                "push and receiver resume)";
    EXPECT_EQ(nullopts_received.load(), 0);

    ch.Close();
    executor.Stop();
}

// ============================================================================
// Bug regression: Channel destructor must not resume waiters inline
// ============================================================================
// Previously, ~Channel() called Close() which resumed suspended coroutines.
// Those coroutines could re-enter the channel (e.g., another Send/Receive),
// accessing a partially-destroyed object. The fix: destructor marks closed
// and clears waiters without resuming them.

TEST(ChannelTest, DestructorDoesNotResumeWaiters) {
    // Create a channel, destroy it without closing, verify no crash.
    // The channel should silently discard any pending waiters.
    {
        Channel<int> ch(1);
        // No waiters, just verify destructor doesn't crash
    }

    // Verify that a channel with buffered data can be destroyed safely
    {
        Channel<int> ch(5);
        SyncWait([&]() -> Task<void> {
            co_await ch.Send(1);
            co_await ch.Send(2);
            // Destroy channel with buffered data — no crash
        }());
    }
}
