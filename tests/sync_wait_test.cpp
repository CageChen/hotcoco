// ============================================================================
// SyncWait Tests
// ============================================================================

#include <gtest/gtest.h>

#include <string>

#include "hotcoco/core/task.hpp"
#include "hotcoco/sync/sync_wait.hpp"

using namespace hotcoco;

// ============================================================================
// Edge Cases
// ============================================================================

TEST(SyncWaitTest, ReturnsString) {
    auto task = []() -> Task<std::string> {
        co_return "sync_wait_result";
    };
    EXPECT_EQ(SyncWait(task()), "sync_wait_result");
}

TEST(SyncWaitTest, MultipleSyncWaits) {
    auto make = [](int x) -> Task<int> { co_return x; };

    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(SyncWait(make(i)), i);
    }
}
