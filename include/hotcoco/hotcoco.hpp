// ============================================================================
// hotcoco/hotcoco.hpp - Main Include Header
// ============================================================================
//
// This convenience header includes the complete hotcoco library.
// For smaller builds, include individual headers as needed.
//
// USAGE:
// ------
//   #include <hotcoco/hotcoco.hpp>
//   using namespace hotcoco;
//
// ============================================================================

#pragma once

// Core primitives
#include "hotcoco/core/concepts.hpp"
#include "hotcoco/core/defer.hpp"
#include "hotcoco/core/detached_task.hpp"
#include "hotcoco/core/error.hpp"
#include "hotcoco/core/generator.hpp"
#include "hotcoco/core/invoke.hpp"
#include "hotcoco/core/result.hpp"
#include "hotcoco/core/schedule_on.hpp"
#include "hotcoco/core/spawn.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/core/task_group.hpp"

// Combinators
#include "hotcoco/core/combinators.hpp"
#include "hotcoco/core/timeout.hpp"
#include "hotcoco/core/when_all.hpp"
#include "hotcoco/core/when_any.hpp"

// Cancellation
#include "hotcoco/core/cancellation.hpp"

// Retry
#include "hotcoco/core/retry.hpp"

// Channels
#include "hotcoco/core/channel.hpp"

// Memory
#include "hotcoco/core/frame_pool.hpp"

// Executors
#include "hotcoco/io/executor.hpp"
#include "hotcoco/io/libuv_executor.hpp"
#include "hotcoco/io/thread_pool_executor.hpp"
#include "hotcoco/io/thread_utils.hpp"

// Timers
#include "hotcoco/io/periodic_timer.hpp"

// Networking
#include "hotcoco/io/sync_tcp.hpp"
#include "hotcoco/io/tcp.hpp"

// io_uring networking
#ifdef HOTCOCO_HAS_IOURING
#include "hotcoco/io/iouring_async_ops.hpp"
#include "hotcoco/io/iouring_tcp.hpp"
#endif

// HTTP
#include "hotcoco/http/http.hpp"

// Synchronization
#include "hotcoco/sync/event.hpp"
#include "hotcoco/sync/latch.hpp"
#include "hotcoco/sync/mutex.hpp"
#include "hotcoco/sync/rwlock.hpp"
#include "hotcoco/sync/semaphore.hpp"
#include "hotcoco/sync/sync_wait.hpp"
