// ============================================================================
// Thread Utilities Tests
// ============================================================================

#include "hotcoco/io/thread_utils.hpp"

#include <gtest/gtest.h>
#include <pthread.h>
#include <sched.h>
#include <thread>

using namespace hotcoco;

// ============================================================================
// CpuAffinity Factory Tests
// ============================================================================

TEST(CpuAffinityTest, NoneIsNotSet) {
    auto affinity = CpuAffinity::None();
    EXPECT_FALSE(affinity.IsSet());
    EXPECT_TRUE(affinity.cpus.empty());
}

TEST(CpuAffinityTest, SingleCore) {
    auto affinity = CpuAffinity::SingleCore(3);
    EXPECT_TRUE(affinity.IsSet());
    ASSERT_EQ(affinity.cpus.size(), 1u);
    EXPECT_EQ(affinity.cpus[0], 3);
}

TEST(CpuAffinityTest, Range) {
    auto affinity = CpuAffinity::Range(2, 5);
    EXPECT_TRUE(affinity.IsSet());
    ASSERT_EQ(affinity.cpus.size(), 4u);
    EXPECT_EQ(affinity.cpus[0], 2);
    EXPECT_EQ(affinity.cpus[1], 3);
    EXPECT_EQ(affinity.cpus[2], 4);
    EXPECT_EQ(affinity.cpus[3], 5);
}

TEST(CpuAffinityTest, RangeSingleElement) {
    auto affinity = CpuAffinity::Range(0, 0);
    ASSERT_EQ(affinity.cpus.size(), 1u);
    EXPECT_EQ(affinity.cpus[0], 0);
}

TEST(CpuAffinityTest, Cores) {
    auto affinity = CpuAffinity::Cores({0, 4, 7});
    EXPECT_TRUE(affinity.IsSet());
    ASSERT_EQ(affinity.cpus.size(), 3u);
    EXPECT_EQ(affinity.cpus[0], 0);
    EXPECT_EQ(affinity.cpus[1], 4);
    EXPECT_EQ(affinity.cpus[2], 7);
}

// ============================================================================
// SetThreadAffinity Tests
// ============================================================================

TEST(ThreadUtilsTest, SetAffinityNoneSucceeds) {
    EXPECT_TRUE(SetThreadAffinity(CpuAffinity::None()));
}

TEST(ThreadUtilsTest, SetAffinitySingleCore) {
    int num_cpus = GetNumCpus();
    ASSERT_GT(num_cpus, 0);

    // Pin to core 0 (always exists)
    EXPECT_TRUE(SetThreadAffinity(CpuAffinity::SingleCore(0)));

    // Verify: the thread should now be on core 0
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    EXPECT_TRUE(CPU_ISSET(0, &cpuset));

    // Restore: allow all cores
    EXPECT_TRUE(SetThreadAffinity(CpuAffinity::Range(0, num_cpus - 1)));
}

TEST(ThreadUtilsTest, SetAffinityRange) {
    int num_cpus = GetNumCpus();
    if (num_cpus < 2) {
        GTEST_SKIP() << "Need at least 2 CPUs for range test";
    }

    EXPECT_TRUE(SetThreadAffinity(CpuAffinity::Range(0, 1)));

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    EXPECT_TRUE(CPU_ISSET(0, &cpuset));
    EXPECT_TRUE(CPU_ISSET(1, &cpuset));

    // Restore
    EXPECT_TRUE(SetThreadAffinity(CpuAffinity::Range(0, num_cpus - 1)));
}

TEST(ThreadUtilsTest, SetAffinityOnWorkerThread) {
    int num_cpus = GetNumCpus();
    ASSERT_GT(num_cpus, 0);

    bool success = false;
    int observed_cpu = -1;

    std::thread worker([&]() {
        success = SetThreadAffinity(CpuAffinity::SingleCore(0));
        if (success) {
            // After pinning, GetCurrentCpu should return 0
            // (may need a moment for migration)
            sched_yield();
            observed_cpu = GetCurrentCpu();
        }
    });
    worker.join();

    EXPECT_TRUE(success);
    EXPECT_EQ(observed_cpu, 0);
}

// ============================================================================
// SetThreadName Tests
// ============================================================================

TEST(ThreadUtilsTest, SetThreadName) {
    EXPECT_TRUE(SetThreadName("test-worker"));

    // Verify via pthread_getname_np
    char name[16] = {};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    EXPECT_STREQ(name, "test-worker");
}

TEST(ThreadUtilsTest, SetThreadNameTruncation) {
    // Linux limits to 15 chars; verify truncation works without failure
    EXPECT_TRUE(SetThreadName("this-is-a-very-long-thread-name"));

    char name[16] = {};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    EXPECT_STREQ(name, "this-is-a-very-");
}

// ============================================================================
// GetNumCpus / GetCurrentCpu Tests
// ============================================================================

TEST(ThreadUtilsTest, GetNumCpusPositive) {
    int n = GetNumCpus();
    EXPECT_GT(n, 0);
}

TEST(ThreadUtilsTest, GetCurrentCpuInRange) {
    int cpu = GetCurrentCpu();
    int num_cpus = GetNumCpus();
    EXPECT_GE(cpu, 0);
    EXPECT_LT(cpu, num_cpus);
}
