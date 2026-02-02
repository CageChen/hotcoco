// ============================================================================
// hotcoco/io/executor.cpp - Executor Thread-Local Implementation
// ============================================================================

#include "hotcoco/io/executor.hpp"

namespace hotcoco {

// Thread-local storage for the current executor
static thread_local Executor* g_current_executor = nullptr;

Executor* GetCurrentExecutor() {
    return g_current_executor;
}

void SetCurrentExecutor(Executor* executor) {
    g_current_executor = executor;
}

ExecutorGuard::ExecutorGuard(Executor* executor)
    : previous_(g_current_executor) {
    g_current_executor = executor;
}

ExecutorGuard::~ExecutorGuard() {
    g_current_executor = previous_;
}

}  // namespace hotcoco
