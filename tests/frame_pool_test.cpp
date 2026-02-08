// ============================================================================
// Frame Pool Tests
// ============================================================================

#include "hotcoco/core/frame_pool.hpp"

#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace hotcoco;

// Under ASan, FramePool bypasses pooling (uses raw new/delete), so
// pool-specific behavior tests (stats, reuse, bucket routing) are skipped.
#define FRAMEPOOL_BYPASS_ACTIVE HOTCOCO_ASAN_ACTIVE

// ============================================================================
// Basic Pool Tests
// ============================================================================

TEST(FramePoolTest, AllocateDeallocate) {
    if (FRAMEPOOL_BYPASS_ACTIVE) {
        GTEST_SKIP() << "FramePool bypassed under ASan";
    }
    FramePool::Clear();
    FramePool::ResetStats();

    void* ptr = FramePool::Allocate(64);
    ASSERT_NE(ptr, nullptr);

    FramePool::Deallocate(ptr, 64);

    auto stats = FramePool::GetStats();
    EXPECT_EQ(stats.allocations, 1);
    EXPECT_EQ(stats.deallocations, 1);
}

TEST(FramePoolTest, PoolReuse) {
    if (FRAMEPOOL_BYPASS_ACTIVE) {
        GTEST_SKIP() << "FramePool bypassed under ASan";
    }
    FramePool::Clear();
    FramePool::ResetStats();

    // Allocate and deallocate
    void* ptr1 = FramePool::Allocate(64);
    FramePool::Deallocate(ptr1, 64);

    // Second allocation should hit the pool
    void* ptr2 = FramePool::Allocate(64);

    // Should get the same pointer back
    EXPECT_EQ(ptr1, ptr2);

    auto stats = FramePool::GetStats();
    EXPECT_EQ(stats.pool_hits, 1);
    EXPECT_EQ(stats.pool_misses, 1);  // First allocation was a miss

    FramePool::Deallocate(ptr2, 64);
}

TEST(FramePoolTest, DifferentSizeBuckets) {
    if (FRAMEPOOL_BYPASS_ACTIVE) {
        GTEST_SKIP() << "FramePool bypassed under ASan";
    }
    FramePool::Clear();
    FramePool::ResetStats();

    // Allocate from different buckets
    void* ptr64 = FramePool::Allocate(64);
    void* ptr128 = FramePool::Allocate(128);
    void* ptr256 = FramePool::Allocate(256);

    EXPECT_NE(ptr64, ptr128);
    EXPECT_NE(ptr128, ptr256);

    FramePool::Deallocate(ptr64, 64);
    FramePool::Deallocate(ptr128, 128);
    FramePool::Deallocate(ptr256, 256);

    // Each should go back to its own bucket
    void* new64 = FramePool::Allocate(64);
    void* new128 = FramePool::Allocate(128);
    void* new256 = FramePool::Allocate(256);

    EXPECT_EQ(new64, ptr64);
    EXPECT_EQ(new128, ptr128);
    EXPECT_EQ(new256, ptr256);

    FramePool::Deallocate(new64, 64);
    FramePool::Deallocate(new128, 128);
    FramePool::Deallocate(new256, 256);
}

TEST(FramePoolTest, OversizedAllocation) {
    if (FRAMEPOOL_BYPASS_ACTIVE) {
        GTEST_SKIP() << "FramePool bypassed under ASan";
    }
    FramePool::Clear();
    FramePool::ResetStats();

    // Allocate something bigger than max bucket
    void* ptr = FramePool::Allocate(4096);
    ASSERT_NE(ptr, nullptr);

    auto stats = FramePool::GetStats();
    EXPECT_EQ(stats.oversized_allocations, 1);

    FramePool::Deallocate(ptr, 4096);
}

TEST(FramePoolTest, PoolLimitRespected) {
    if (FRAMEPOOL_BYPASS_ACTIVE) {
        GTEST_SKIP() << "FramePool bypassed under ASan";
    }
    FramePool::Clear();
    FramePool::ResetStats();

    constexpr size_t count = FramePool::kMaxPooledPerBucket + 10;
    std::vector<void*> ptrs;

    // Allocate more than pool limit
    for (size_t i = 0; i < count; i++) {
        ptrs.push_back(FramePool::Allocate(64));
    }

    // Deallocate all - only kMaxPooledPerBucket should be pooled
    for (void* ptr : ptrs) {
        FramePool::Deallocate(ptr, 64);
    }

    // Allocate again - should get kMaxPooledPerBucket hits
    FramePool::ResetStats();

    for (size_t i = 0; i < FramePool::kMaxPooledPerBucket; i++) {
        FramePool::Allocate(64);
    }

    auto stats = FramePool::GetStats();
    EXPECT_EQ(stats.pool_hits, FramePool::kMaxPooledPerBucket);
}

// ============================================================================
// PooledPromise Tests
// ============================================================================

struct TestPromise : PooledPromise {
    int data = 42;
};

TEST(FramePoolTest, PooledPromiseMixin) {
    if (FRAMEPOOL_BYPASS_ACTIVE) {
        GTEST_SKIP() << "FramePool bypassed under ASan";
    }
    FramePool::Clear();
    FramePool::ResetStats();

    TestPromise* p = new TestPromise();
    EXPECT_EQ(p->data, 42);
    delete p;

    auto stats = FramePool::GetStats();
    EXPECT_EQ(stats.allocations, 1);
    EXPECT_EQ(stats.deallocations, 1);
}

// ============================================================================
// Thread Safety Tests
// ============================================================================

TEST(FramePoolTest, ThreadLocalPools) {
    // Each thread should have its own pool
    std::atomic<void*> ptr1{nullptr};
    std::atomic<void*> ptr2{nullptr};

    std::thread t1([&] {
        FramePool::Clear();
        void* p = FramePool::Allocate(64);
        FramePool::Deallocate(p, 64);
        ptr1 = FramePool::Allocate(64);
        FramePool::Deallocate(ptr1.load(), 64);
    });

    std::thread t2([&] {
        FramePool::Clear();
        void* p = FramePool::Allocate(64);
        FramePool::Deallocate(p, 64);
        ptr2 = FramePool::Allocate(64);
        FramePool::Deallocate(ptr2.load(), 64);
    });

    t1.join();
    t2.join();

    // Each thread reused its own pooled pointer
    // (We can't easily verify they're different due to thread-local pools)
    SUCCEED();
}

// ============================================================================
// Additional Edge Cases
// ============================================================================

#if !FRAMEPOOL_BYPASS_ACTIVE

TEST(FramePoolTest, ClearReleasesMemory) {
    FramePool::Clear();
    FramePool::ResetStats();

    void* p1 = FramePool::Allocate(64);
    FramePool::Deallocate(p1, 64);

    // Pool should have one entry
    FramePool::Clear();

    // After clear, next allocation should miss
    FramePool::ResetStats();
    void* p2 = FramePool::Allocate(64);
    auto stats = FramePool::GetStats();
    EXPECT_EQ(stats.pool_misses, 1u);
    EXPECT_EQ(stats.pool_hits, 0u);

    FramePool::Deallocate(p2, 64);
}

TEST(FramePoolTest, ZeroSizeAllocation) {
    FramePool::Clear();
    FramePool::ResetStats();

    // Zero-size should still work
    void* p = FramePool::Allocate(0);
    EXPECT_NE(p, nullptr);
    FramePool::Deallocate(p, 0);
}

TEST(FramePoolTest, MultipleAllocSameSize) {
    FramePool::Clear();
    FramePool::ResetStats();

    std::vector<void*> ptrs;
    for (int i = 0; i < 10; ++i) {
        ptrs.push_back(FramePool::Allocate(128));
    }

    for (void* p : ptrs) {
        FramePool::Deallocate(p, 128);
    }

    auto stats = FramePool::GetStats();
    EXPECT_EQ(stats.allocations, 10u);
    EXPECT_EQ(stats.deallocations, 10u);
}

#endif  // !FRAMEPOOL_BYPASS_ACTIVE
