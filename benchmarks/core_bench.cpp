// ============================================================================
// Core Primitives Benchmarks
// ============================================================================
//
// Baseline results (2026-02-10, GCC Release, AMD Ryzen 9 9950X 16C/32T @ 6.0GHz, FramePool enabled):
//
//   Benchmark                          Time        CPU      Iterations
//   -----------------------------------------------------------------
//   BM_TaskCreate                     2.51 ns     2.51 ns   279773727
//   BM_TaskSyncWait                   19.0 ns     19.0 ns    36919320
//   BM_TaskChainedAwait               39.9 ns     39.9 ns    17445639
//   BM_GeneratorIterate/10            15.3 ns     15.3 ns    45839436  655M items/s
//   BM_GeneratorIterate/64             114 ns      114 ns     6396331  563M items/s
//   BM_GeneratorIterate/512            774 ns      774 ns      903302  662M items/s
//   BM_GeneratorIterate/4096          6056 ns     6055 ns      115681  676M items/s
//   BM_GeneratorIterate/10000        14789 ns    14788 ns       47527  676M items/s
//   BM_RawFunctionCall                0.087 ns   0.087 ns  1000000000
//   BM_TaskFunctionCall               19.0 ns     19.0 ns    37065672
//   BM_ResultOkCreate                0.178 ns    0.178 ns  1000000000
//   BM_ResultErrCreate               0.265 ns    0.265 ns  1000000000
//   BM_ResultMap                     0.177 ns    0.177 ns  1000000000
//   BM_ResultAndThen                 0.178 ns    0.178 ns  1000000000
//   BM_WhenAllVariadic                65.4 ns     65.4 ns    10729249
//   BM_WhenAllVector/2                77.8 ns     77.8 ns     8991628
//   BM_WhenAllVector/8                 151 ns      151 ns     4635773
//   BM_WhenAllVector/64                852 ns      852 ns      817749
//   BM_WhenAllVector/256              7949 ns     7948 ns       88397
//   BM_WhenAnyVariadic                 136 ns      136 ns     5169281
//   BM_WhenAnyVector/2                 131 ns      131 ns     5361361
//   BM_WhenAnyVector/8                 274 ns      274 ns     2557186
//   BM_WhenAnyVector/64               1919 ns     1919 ns      360300
//   BM_WhenAnyVector/256             13855 ns    13853 ns       50664
//   BM_SpawnAndComplete                534 ns      534 ns     1298760
//
// ============================================================================

#include <benchmark/benchmark.h>

#include "hotcoco/core/task.hpp"
#include "hotcoco/core/generator.hpp"
#include "hotcoco/core/result.hpp"
#include "hotcoco/core/spawn.hpp"
#include "hotcoco/core/when_all.hpp"
#include "hotcoco/core/when_any.hpp"
#include "hotcoco/io/libuv_executor.hpp"
#include "hotcoco/sync/sync_wait.hpp"

using namespace hotcoco;

// ============================================================================
// Task Benchmarks
// ============================================================================

// Measure Task creation and destruction overhead
static void BM_TaskCreate(benchmark::State& state) {
    for (auto _ : state) {
        auto task = []() -> Task<int> { co_return 42; }();
        benchmark::DoNotOptimize(task);
    }
}
BENCHMARK(BM_TaskCreate);

// Measure SyncWait overhead for simple task
static void BM_TaskSyncWait(benchmark::State& state) {
    for (auto _ : state) {
        int result = SyncWait([]() -> Task<int> { co_return 42; }());
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_TaskSyncWait);

// Measure chained awaits
static void BM_TaskChainedAwait(benchmark::State& state) {
    auto inner = []() -> Task<int> { co_return 42; };
    auto outer = [&inner]() -> Task<int> {
        int a = co_await inner();
        int b = co_await inner();
        int c = co_await inner();
        co_return a + b + c;
    };
    
    for (auto _ : state) {
        int result = SyncWait(outer());
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_TaskChainedAwait);

// ============================================================================
// Generator Benchmarks
// ============================================================================

static Generator<int> CountTo(int n) {
    for (int i = 0; i < n; ++i) {
        co_yield i;
    }
}

static void BM_GeneratorIterate(benchmark::State& state) {
    const int count = state.range(0);
    for (auto _ : state) {
        int sum = 0;
        for (int val : CountTo(count)) {
            sum += val;
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_GeneratorIterate)->Range(10, 10000);

// ============================================================================
// Task vs Raw Function Call
// ============================================================================

static int RawAdd(int a, int b) { return a + b; }

static Task<int> TaskAdd(int a, int b) { co_return a + b; }

static void BM_RawFunctionCall(benchmark::State& state) {
    for (auto _ : state) {
        int result = RawAdd(1, 2);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_RawFunctionCall);

static void BM_TaskFunctionCall(benchmark::State& state) {
    for (auto _ : state) {
        int result = SyncWait(TaskAdd(1, 2));
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_TaskFunctionCall);

// ============================================================================
// Result Benchmarks
// ============================================================================

static void BM_ResultOkCreate(benchmark::State& state) {
    for (auto _ : state) {
        auto result = Result<int, std::error_code>(Ok(42));
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_ResultOkCreate);

static void BM_ResultErrCreate(benchmark::State& state) {
    for (auto _ : state) {
        auto result = Result<int, std::error_code>(
            Err(make_error_code(std::errc::invalid_argument)));
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_ResultErrCreate);

static void BM_ResultMap(benchmark::State& state) {
    for (auto _ : state) {
        auto result = Result<int, std::error_code>(Ok(42));
        auto mapped = std::move(result).Map([](int v) { return v * 2; });
        benchmark::DoNotOptimize(mapped);
    }
}
BENCHMARK(BM_ResultMap);

static void BM_ResultAndThen(benchmark::State& state) {
    for (auto _ : state) {
        auto result = Result<int, std::error_code>(Ok(42));
        auto chained = std::move(result).AndThen(
            [](int v) -> Result<int, std::error_code> { return Ok(v + 1); });
        benchmark::DoNotOptimize(chained);
    }
}
BENCHMARK(BM_ResultAndThen);

// ============================================================================
// WhenAll Benchmarks
// ============================================================================

static void BM_WhenAllVariadic(benchmark::State& state) {
    for (auto _ : state) {
        auto result = SyncWait([]() -> Task<std::tuple<int, int, int>> {
            co_return co_await WhenAll(
                []() -> Task<int> { co_return 1; }(),
                []() -> Task<int> { co_return 2; }(),
                []() -> Task<int> { co_return 3; }());
        }());
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_WhenAllVariadic);

static void BM_WhenAllVector(benchmark::State& state) {
    const int count = state.range(0);
    for (auto _ : state) {
        auto result = SyncWait([count]() -> Task<std::vector<int>> {
            std::vector<Task<int>> tasks;
            tasks.reserve(count);
            for (int i = 0; i < count; ++i) {
                tasks.push_back([i]() -> Task<int> { co_return i; }());
            }
            co_return co_await WhenAll(std::move(tasks));
        }());
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_WhenAllVector)->Range(2, 256);

// ============================================================================
// WhenAny Benchmarks
// ============================================================================

static void BM_WhenAnyVariadic(benchmark::State& state) {
    for (auto _ : state) {
        auto result = SyncWait([]() -> Task<Result<WhenAnyResult<int>, std::error_code>> {
            co_return co_await WhenAny(
                []() -> Task<int> { co_return 1; }(),
                []() -> Task<int> { co_return 2; }());
        }());
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_WhenAnyVariadic);

static void BM_WhenAnyVector(benchmark::State& state) {
    const int count = state.range(0);
    for (auto _ : state) {
        auto result = SyncWait([count]() -> Task<Result<WhenAnyResult<int>, std::error_code>> {
            std::vector<Task<int>> tasks;
            tasks.reserve(count);
            for (int i = 0; i < count; ++i) {
                tasks.push_back([i]() -> Task<int> { co_return i; }());
            }
            co_return co_await WhenAny(std::move(tasks));
        }());
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_WhenAnyVector)->Range(2, 256);

// ============================================================================
// Spawn Benchmarks
// ============================================================================

static void BM_SpawnAndComplete(benchmark::State& state) {
    auto executor_ptr = LibuvExecutor::Create().Value();
    auto& executor = *executor_ptr;

    for (auto _ : state) {
        std::atomic<bool> done{false};
        Spawn(executor, []() -> Task<void> { co_return; }())
            .OnComplete([&done]() { done.store(true, std::memory_order_release); });
        executor.RunOnce();
        while (!done.load(std::memory_order_acquire)) {
            executor.RunOnce();
        }
    }
}
BENCHMARK(BM_SpawnAndComplete);
