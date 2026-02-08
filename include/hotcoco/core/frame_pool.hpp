// ============================================================================
// hotcoco/core/frame_pool.hpp - Coroutine Frame Pooling
// ============================================================================
//
// FramePool provides efficient reuse of coroutine frame allocations.
// Instead of calling new/delete for each coroutine, frames are pooled
// and reused, reducing allocation overhead significantly.
//
// DESIGN PHILOSOPHY:
// ------------------
// 1. THREAD-LOCAL POOLS: No contention in common case
// 2. SIZE-BUCKETED: Different pools for different frame sizes
// 3. TRANSPARENT: Use via operator new/delete in promise_type
//
// USAGE:
// ------
//   // In your Task promise_type:
//   void* operator new(std::size_t size) {
//       return FramePool::Allocate(size);
//   }
//   void operator delete(void* ptr, std::size_t size) {
//       FramePool::Deallocate(ptr, size);
//   }
//
// ============================================================================

#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <vector>

// GCC uses __SANITIZE_ADDRESS__, Clang uses __has_feature(address_sanitizer).
// __has_feature cannot appear in a single #if with || on GCC (preprocessor error).
#if defined(__SANITIZE_ADDRESS__)
#define HOTCOCO_ASAN_ACTIVE 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HOTCOCO_ASAN_ACTIVE 1
#endif
#endif
#ifndef HOTCOCO_ASAN_ACTIVE
#define HOTCOCO_ASAN_ACTIVE 0
#endif

namespace hotcoco {

// ============================================================================
// FramePool - Thread-local coroutine frame allocator
// ============================================================================
//
// NOTE: FramePool uses thread_local storage. If a coroutine is created on
// thread A but destroyed on thread B (cross-thread migration), the frame
// is returned to thread B's pool. This is safe (the underlying memory
// comes from ::operator new) but means thread A's pool doesn't reclaim
// the slot. This is acceptable for hotcoco's single-threaded executor
// model where coroutines don't migrate between threads.
//
class FramePool {
   public:
    // Size buckets: 64, 128, 256, 512, 1024, 2048 bytes
    static constexpr size_t kNumBuckets = 6;
    static constexpr size_t kMinBucketSize = 64;
    static constexpr size_t kMaxBucketSize = 2048;
    static constexpr size_t kMaxPooledPerBucket = 64;

    // Get thread-local pool instance
    static FramePool& Instance() {
        thread_local FramePool pool;
        return pool;
    }

    // Allocate a frame of the given size
    static void* Allocate(size_t size) {
#if HOTCOCO_ASAN_ACTIVE
        return ::operator new(size);
#else
        return Instance().DoAllocate(size);
#endif
    }

    // Deallocate a frame
    static void Deallocate(void* ptr, size_t size) {
#if HOTCOCO_ASAN_ACTIVE
        ::operator delete(ptr, size);
#else
        Instance().DoDeallocate(ptr, size);
#endif
    }

    // Get statistics
    struct Stats {
        size_t allocations = 0;
        size_t deallocations = 0;
        size_t pool_hits = 0;
        size_t pool_misses = 0;
        size_t oversized_allocations = 0;
    };

    static Stats GetStats() { return Instance().stats_; }

    static void ResetStats() { Instance().stats_ = Stats{}; }

    // Clear all pooled frames (for testing/cleanup)
    static void Clear() { Instance().DoClear(); }

    ~FramePool() { DoClear(); }

   private:
    FramePool() = default;

    // Non-copyable, non-movable
    FramePool(const FramePool&) = delete;
    FramePool& operator=(const FramePool&) = delete;

    void* DoAllocate(size_t size) {
        stats_.allocations++;

        int bucket = GetBucket(size);
        if (bucket < 0) {
            // Oversized - use regular allocation
            stats_.oversized_allocations++;
            return ::operator new(size);
        }

        auto& pool = buckets_[static_cast<size_t>(bucket)];
        if (!pool.empty()) {
            void* ptr = pool.back();
            pool.pop_back();
            stats_.pool_hits++;
            return ptr;
        }

        // Pool empty - allocate new
        stats_.pool_misses++;
        return ::operator new(GetBucketSize(bucket));
    }

    void DoDeallocate(void* ptr, size_t size) {
        stats_.deallocations++;

        if (!ptr) return;

        int bucket = GetBucket(size);
        if (bucket < 0) {
            // Oversized - use regular deallocation
            ::operator delete(ptr);
            return;
        }

        auto& pool = buckets_[static_cast<size_t>(bucket)];
        if (pool.size() < kMaxPooledPerBucket) {
            pool.push_back(ptr);
        } else {
            // Pool full - actually free
            ::operator delete(ptr);
        }
    }

    void DoClear() {
        for (auto& pool : buckets_) {
            for (void* ptr : pool) {
                ::operator delete(ptr);
            }
            pool.clear();
        }
    }

    // Map size to bucket index. Returns -1 if oversized.
    static int GetBucket(size_t size) {
        if (size <= 64) return 0;
        if (size <= 128) return 1;
        if (size <= 256) return 2;
        if (size <= 512) return 3;
        if (size <= 1024) return 4;
        if (size <= 2048) return 5;
        return -1;  // Oversized
    }

    static size_t GetBucketSize(int bucket) {
        static constexpr size_t sizes[] = {64, 128, 256, 512, 1024, 2048};
        return sizes[bucket];
    }

    std::array<std::vector<void*>, kNumBuckets> buckets_;
    Stats stats_;
};

// ============================================================================
// PooledPromise - Mixin for promise_type to use pooled allocation
// ============================================================================
//
// Add this to your promise_type:
//
//   struct promise_type : PooledPromise {
//       // ... rest of promise
//   };
//
// Or manually add operator new/delete using FramePool.

struct PooledPromise {
    void* operator new(std::size_t size) { return FramePool::Allocate(size); }

    void operator delete(void* ptr, std::size_t size) { FramePool::Deallocate(ptr, size); }
};

}  // namespace hotcoco
