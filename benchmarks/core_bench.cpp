// ============================================================================
// Core Primitives Benchmarks
// ============================================================================
//
// Baseline results (2026-02-08, GCC Release, AMD Ryzen 9 9950X 16C/32T @ 6.0GHz):
//
//   Benchmark                          Time        CPU      Iterations
//   -----------------------------------------------------------------
//   BM_TaskCreate                     5.38 ns     5.38 ns   129041452
//   BM_TaskSyncWait                   23.3 ns     23.3 ns    29960504
//   BM_TaskChainedAwait               48.9 ns     48.9 ns    14166319
//   BM_GeneratorIterate/10            17.7 ns     17.7 ns    40729117  564M items/s
//   BM_GeneratorIterate/64             114 ns      114 ns     5932432  561M items/s
//   BM_GeneratorIterate/512            762 ns      762 ns      924026  672M items/s
//   BM_GeneratorIterate/4096          5967 ns     5967 ns      117810  686M items/s
//   BM_GeneratorIterate/10000        14530 ns    14529 ns       48518  688M items/s
//   BM_RawFunctionCall                0.087 ns   0.087 ns  1000000000
//   BM_TaskFunctionCall               23.9 ns     23.9 ns    29255558
//   BM_ResultOkCreate                0.178 ns    0.178 ns  1000000000
//   BM_ResultErrCreate               0.265 ns    0.265 ns  1000000000
//   BM_ResultMap                     0.177 ns    0.177 ns  1000000000
//   BM_ResultAndThen                 0.178 ns    0.178 ns  1000000000
//   BM_WhenAllVariadic                88.0 ns     88.0 ns     7939150
//   BM_WhenAllVector/2                92.6 ns     92.6 ns     7569510
//   BM_WhenAllVector/8                 240 ns      240 ns     2943629
//   BM_WhenAllVector/64               1390 ns     1390 ns      501021
//   BM_WhenAllVector/256              9890 ns     9889 ns       70803
//   BM_WhenAnyVariadic                 152 ns      152 ns     4617499
//   BM_WhenAnyVector/2                 147 ns      147 ns     4731955
//   BM_WhenAnyVector/8                 373 ns      373 ns     1879308
//   BM_WhenAnyVector/64               2283 ns     2283 ns      304062
//   BM_WhenAnyVector/256             14135 ns    14135 ns       49291
//   BM_SpawnAndComplete                513 ns      513 ns     1293716
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
