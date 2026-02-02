// ============================================================================
// hotcoco/io/thread_utils.hpp - Thread Utilities
// ============================================================================
//
// Threading utilities for CPU affinity, naming, and priority.
// Used by executors to configure worker threads for optimal performance.
//
// USAGE:
// ------
//   // Bind current thread to core 0
//   SetThreadAffinity(CpuAffinity::SingleCore(0));
//
//   // Set thread name (visible in htop, top, etc.)
//   SetThreadName("my-worker");
//
//   // Set thread priority (nice value)
//   SetThreadNice(-5);  // Higher priority
//
// ============================================================================

#pragma once

#include <string>
#include <vector>

namespace hotcoco {

// ============================================================================
// CpuAffinity - CPU Core Binding Configuration
// ============================================================================
struct CpuAffinity {
    std::vector<int> cpus;

    // ========================================================================
    // Factory Methods
    // ========================================================================

    // No affinity restriction (thread can run on any core)
    static CpuAffinity None() { return CpuAffinity{}; }

    // Bind to a single core
    static CpuAffinity SingleCore(int cpu) { return CpuAffinity{{cpu}}; }

    // Bind to a range of cores [start, end]
    static CpuAffinity Range(int start, int end) {
        CpuAffinity affinity;
        for (int i = start; i <= end; ++i) {
            affinity.cpus.push_back(i);
        }
        return affinity;
    }

    // Bind to specific cores
    static CpuAffinity Cores(std::vector<int> cores) {
        return CpuAffinity{std::move(cores)};
    }

    // Check if affinity is set
    bool IsSet() const { return !cpus.empty(); }
};

// ============================================================================
// Thread Utility Functions
// ============================================================================

// Apply CPU affinity to the current thread
// Returns true on success, false on failure
bool SetThreadAffinity(const CpuAffinity& affinity);

// Set the name of the current thread (max 15 chars on Linux)
// Visible in htop, top, /proc/[pid]/comm, etc.
bool SetThreadName(const std::string& name);

// Set the nice value of the current thread
// Range: -20 (highest priority) to 19 (lowest priority)
// Requires root/CAP_SYS_NICE for negative values
bool SetThreadNice(int nice_value);

// Get the number of CPUs available on the system
int GetNumCpus();

// Get the CPU the current thread is running on
int GetCurrentCpu();

}  // namespace hotcoco
