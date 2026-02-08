// ============================================================================
// Defer Tests
// ============================================================================

#include "hotcoco/core/defer.hpp"

#include <gtest/gtest.h>
#include <string>

using namespace hotcoco;

// ============================================================================
// Basic Defer Tests
// ============================================================================

TEST(DeferTest, RunsOnScopeExit) {
    bool executed = false;
    {
        Defer d([&] { executed = true; });
        EXPECT_FALSE(executed);
    }
    EXPECT_TRUE(executed);
}

TEST(DeferTest, MultipleDefers) {
    std::string order;
    {
        Defer d1([&] { order += "1"; });
        Defer d2([&] { order += "2"; });
        Defer d3([&] { order += "3"; });
    }
    // LIFO order: last registered runs first
    EXPECT_EQ(order, "321");
}

TEST(DeferTest, Cancel) {
    bool executed = false;
    {
        Defer d([&] { executed = true; });
        d.Cancel();
    }
    EXPECT_FALSE(executed);
}

TEST(DeferTest, ExecuteNow) {
    int count = 0;
    {
        Defer d([&] { count++; });
        EXPECT_EQ(count, 0);
        d.ExecuteNow();
        EXPECT_EQ(count, 1);
    }
    // Should not run again at scope exit
    EXPECT_EQ(count, 1);
}

TEST(DeferTest, MoveConstruction) {
    bool executed = false;
    {
        Defer d1([&] { executed = true; });
        Defer d2(std::move(d1));
    }
    EXPECT_TRUE(executed);
}

// ============================================================================
// DEFER Macro Tests
// ============================================================================

TEST(DeferTest, DeferMacro) {
    bool executed = false;
    {
        DEFER([&] { executed = true; });
        EXPECT_FALSE(executed);
    }
    EXPECT_TRUE(executed);
}

TEST(DeferTest, MultipleDeferMacros) {
    std::string order;
    {
        DEFER([&] { order += "1"; });
        DEFER([&] { order += "2"; });
        DEFER([&] { order += "3"; });
    }
    EXPECT_EQ(order, "321");
}

// ============================================================================
// Additional Edge Cases
// ============================================================================

TEST(DeferTest, MoveAssignment) {
    int count = 0;
    {
        Defer d1([&] { count += 1; });
        Defer d2([&] { count += 10; });

        // Move-assign d2 over d1 — d1's action runs immediately
        d1 = std::move(d2);
    }
    // d1 (originally d2) runs at scope exit: +10
    // d1's original action ran during move-assign: +1
    EXPECT_EQ(count, 11);
}

TEST(DeferTest, ExecuteNowThenCancel) {
    int count = 0;
    {
        Defer d([&] { count++; });
        d.ExecuteNow();
        EXPECT_EQ(count, 1);
        d.Cancel();  // No-op since already executed
    }
    EXPECT_EQ(count, 1);
}

TEST(DeferTest, CancelThenExecuteNow) {
    int count = 0;
    {
        Defer d([&] { count++; });
        d.Cancel();
        d.ExecuteNow();  // Should be no-op
    }
    EXPECT_EQ(count, 0);
}
