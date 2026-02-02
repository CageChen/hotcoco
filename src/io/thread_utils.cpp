// ============================================================================
// hotcoco/io/thread_utils.cpp - Thread Utilities Implementation
// ============================================================================

#include "hotcoco/io/thread_utils.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace hotcoco {

bool SetThreadAffinity(const CpuAffinity& affinity) {
    if (!affinity.IsSet()) {
        return true;  // No affinity to set
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    for (int cpu : affinity.cpus) {
        if (cpu >= 0 && cpu < CPU_SETSIZE) {
            CPU_SET(cpu, &cpuset);
        }
    }

    pthread_t thread = pthread_self();
    return pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset) == 0;
}

bool SetThreadName(const std::string& name) {
    // Linux limits thread names to 15 chars + null terminator
    std::string truncated = name.substr(0, 15);
    return pthread_setname_np(pthread_self(), truncated.c_str()) == 0;
}

bool SetThreadNice(int nice_value) {
    // Clamp to valid range
    nice_value = std::max(-20, std::min(19, nice_value));

    // Get thread ID (for setpriority)
    pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    return setpriority(PRIO_PROCESS, tid, nice_value) == 0;
}

int GetNumCpus() {
    return static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
}

int GetCurrentCpu() {
    return sched_getcpu();
}

}  // namespace hotcoco
