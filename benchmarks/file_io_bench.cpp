// ============================================================================
// File I/O Benchmarks — Multi-IODepth (O_DIRECT, 1 GB file, random offsets)
// ============================================================================
//
// Compares io_uring (WhenAll, single thread) vs sync (N pre-created threads)
// for concurrent 4K O_DIRECT random I/O on a 1 GB test file.
//
// Run with:
//   ./build/benchmarks/hotcoco_bench --benchmark_filter="File|Rand"
//
// NOTE: A 1 GB test file is created on first run at /tmp/hotcoco_file_bench/.
//       Ensure sufficient disk space.
//
// ----------------------------------------------------------------------------
// Reference results (Release, 32-core, 5.7 GHz, NVMe, 1 GB O_DIRECT file):
//
//   Multi-iodepth 4K random read (io_uring WhenAll 1T vs sync N threads):
//                  io_uring       sync           winner
//     depth  1:    1.1 GiB/s      3.1 GiB/s     sync 2.9x
//     depth  4:    3.3 GiB/s      6.2 GiB/s     sync 1.9x
//     depth 16:    5.2 GiB/s     11.5 GiB/s     sync 2.2x
//     depth 64:    6.4 GiB/s     29.6 GiB/s     sync 4.6x
//
//   Multi-iodepth 4K random write (io_uring WhenAll 1T vs sync N threads):
//                  io_uring       sync           winner
//     depth  1:    1.1 GiB/s      2.9 GiB/s     sync 2.5x
//     depth  4:    2.9 GiB/s      6.7 GiB/s     sync 2.3x
//     depth 16:    5.1 GiB/s     11.7 GiB/s     sync 2.3x
//     depth 64:    6.4 GiB/s     37.8 GiB/s     sync 5.9x
// ============================================================================

#ifdef HOTCOCO_HAS_IOURING

#include "hotcoco/core/task.hpp"
#include "hotcoco/core/when_all.hpp"
#include "hotcoco/io/iouring_executor.hpp"
#include "hotcoco/io/iouring_file.hpp"

#include <sys/stat.h>

#include <atomic>
#include <barrier>
#include <benchmark/benchmark.h>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace hotcoco;

namespace {

// ============================================================================
// Constants
// ============================================================================

const std::string kBenchDir = "/tmp/hotcoco_file_bench";
constexpr size_t kTestFileSize = 1ULL * 1024 * 1024 * 1024;  // 1 GB
constexpr size_t kBlockSize = 4096;

// ============================================================================
// Helpers
// ============================================================================

// Aligned buffer for O_DIRECT (must be 4K-aligned)
struct AlignedBuffer {
    void* ptr = nullptr;
    size_t size = 0;

    explicit AlignedBuffer(size_t sz) : size(sz) {
        ptr = aligned_alloc(4096, (sz + 4095) & ~size_t{4095});
        std::memset(ptr, 'X', sz);
    }

    ~AlignedBuffer() { free(ptr); }

    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    AlignedBuffer(AlignedBuffer&& other) noexcept : ptr(other.ptr), size(other.size) {
        other.ptr = nullptr;
        other.size = 0;
    }
    AlignedBuffer& operator=(AlignedBuffer&& other) noexcept {
        if (this != &other) {
            free(ptr);
            ptr = other.ptr;
            size = other.size;
            other.ptr = nullptr;
            other.size = 0;
        }
        return *this;
    }
};

// Fast xorshift64 RNG for generating random offsets
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed = 42) : state(seed) {}

    uint64_t Next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }

    // Returns a random aligned offset within [0, file_size - block_size]
    uint64_t RandOffset(size_t file_size, size_t block_size) {
        size_t max_blocks = file_size / block_size;
        return (Next() % max_blocks) * block_size;
    }
};

// Run a Task<void> coroutine to completion on an IoUringExecutor.
void RunTask(IoUringExecutor& executor, Task<void> task) {
    executor.Schedule(task.GetHandle());
    executor.Run();
}

// Create a large O_DIRECT test file (idempotent: skips if already exists)
void EnsureLargeTestFile(const std::string& path, size_t target_size) {
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0 && static_cast<size_t>(st.st_size) == target_size) {
        return;
    }

    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);

    constexpr size_t kChunkSize = 1024 * 1024;  // 1 MB write chunks
    AlignedBuffer buf(kChunkSize);

    size_t written = 0;
    while (written < target_size) {
        size_t chunk = std::min(kChunkSize, target_size - written);
        ::pwrite(fd, buf.ptr, chunk, static_cast<off_t>(written));
        written += chunk;
    }
    ::fdatasync(fd);
    ::close(fd);
}

// Ensure bench dir + test file exist
void EnsureSetup() {
    std::filesystem::create_directories(kBenchDir);
    EnsureLargeTestFile(kBenchDir + "/testdata.bin", kTestFileSize);
}

std::string TestFilePath() {
    return kBenchDir + "/testdata.bin";
}

// ============================================================================
// io_uring helpers (free coroutine functions to avoid lambda capture issues)
// ============================================================================

Task<void> ReadOneBlockAt(File& file, void* buf, size_t size, uint64_t offset) {
    auto n = co_await file.ReadAt(buf, size, offset);
    benchmark::DoNotOptimize(n);
}

Task<void> WriteOneBlockAt(File& file, const void* buf, size_t size, uint64_t offset) {
    auto n = co_await file.WriteAt(buf, size, offset);
    benchmark::DoNotOptimize(n);
}

// ============================================================================
// io_uring Multi-IODepth Random Read (WhenAll, single thread)
// ============================================================================

Task<void> RunMultiDepthRandRead(IoUringExecutor& executor, const std::string& path, size_t iodepth,
                                 std::vector<AlignedBuffer>& bufs, Rng& rng) {
    auto result = co_await File::Open(path, O_RDONLY | O_DIRECT);
    auto file = std::move(result).Value();

    std::vector<Task<void>> tasks;
    tasks.reserve(iodepth);
    for (size_t i = 0; i < iodepth; ++i) {
        uint64_t offset = rng.RandOffset(kTestFileSize, kBlockSize);
        tasks.push_back(ReadOneBlockAt(file, bufs[i].ptr, kBlockSize, offset));
    }
    co_await WhenAll(std::move(tasks));
    co_await file.Close();
    executor.Stop();
}

static void BM_IoUringMultiRandRead(benchmark::State& state) {
    const auto iodepth = static_cast<size_t>(state.range(0));
    EnsureSetup();

    auto res = IoUringExecutor::Create();
    auto& executor = *res.Value();

    std::vector<AlignedBuffer> bufs;
    bufs.reserve(iodepth);
    for (size_t i = 0; i < iodepth; ++i) {
        bufs.emplace_back(kBlockSize);
    }

    Rng rng(11111);
    const auto path = TestFilePath();

    for (auto _ : state) {
        auto task = RunMultiDepthRandRead(executor, path, iodepth, bufs, rng);
        RunTask(executor, std::move(task));
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(iodepth) *
                            static_cast<int64_t>(kBlockSize));
    state.counters["iodepth"] = static_cast<double>(iodepth);
}
BENCHMARK(BM_IoUringMultiRandRead)->Arg(1)->Arg(4)->Arg(16)->Arg(64);

// ============================================================================
// io_uring Multi-IODepth Random Write (WhenAll, single thread)
// ============================================================================

Task<void> RunMultiDepthRandWrite(IoUringExecutor& executor, const std::string& path, size_t iodepth,
                                  std::vector<AlignedBuffer>& bufs, Rng& rng) {
    auto result = co_await File::Open(path, O_RDWR | O_DIRECT);
    auto file = std::move(result).Value();

    std::vector<Task<void>> tasks;
    tasks.reserve(iodepth);
    for (size_t i = 0; i < iodepth; ++i) {
        uint64_t offset = rng.RandOffset(kTestFileSize, kBlockSize);
        tasks.push_back(WriteOneBlockAt(file, bufs[i].ptr, kBlockSize, offset));
    }
    co_await WhenAll(std::move(tasks));
    co_await file.Close();
    executor.Stop();
}

static void BM_IoUringMultiRandWrite(benchmark::State& state) {
    const auto iodepth = static_cast<size_t>(state.range(0));
    EnsureSetup();

    auto res = IoUringExecutor::Create();
    auto& executor = *res.Value();

    std::vector<AlignedBuffer> bufs;
    bufs.reserve(iodepth);
    for (size_t i = 0; i < iodepth; ++i) {
        bufs.emplace_back(kBlockSize);
    }

    Rng rng(22222);
    const auto path = TestFilePath();

    for (auto _ : state) {
        auto task = RunMultiDepthRandWrite(executor, path, iodepth, bufs, rng);
        RunTask(executor, std::move(task));
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(iodepth) *
                            static_cast<int64_t>(kBlockSize));
    state.counters["iodepth"] = static_cast<double>(iodepth);
}
BENCHMARK(BM_IoUringMultiRandWrite)->Arg(1)->Arg(4)->Arg(16)->Arg(64);

// ============================================================================
// Sync Multi-IODepth Random Read (N pre-created threads)
// ============================================================================

static void BM_SyncMultiRandRead(benchmark::State& state) {
    const auto iodepth = static_cast<size_t>(state.range(0));
    EnsureSetup();
    const auto path = TestFilePath();

    std::vector<AlignedBuffer> bufs;
    bufs.reserve(iodepth);
    for (size_t i = 0; i < iodepth; ++i) {
        bufs.emplace_back(kBlockSize);
    }

    // Pre-open one fd per thread
    std::vector<int> fds(iodepth);
    for (size_t i = 0; i < iodepth; ++i) {
        fds[i] = ::open(path.c_str(), O_RDONLY | O_DIRECT);
    }

    Rng rng(33333);
    std::vector<uint64_t> offsets(iodepth);
    std::atomic<bool> running{true};

    std::barrier start_barrier(static_cast<std::ptrdiff_t>(iodepth + 1));
    std::barrier done_barrier(static_cast<std::ptrdiff_t>(iodepth + 1));

    // Pre-create worker threads
    std::vector<std::thread> threads;
    threads.reserve(iodepth);
    for (size_t i = 0; i < iodepth; ++i) {
        threads.emplace_back([&, i]() {
            while (true) {
                start_barrier.arrive_and_wait();
                if (!running.load(std::memory_order_relaxed)) {
                    return;
                }
                auto n = ::pread(fds[i], bufs[i].ptr, kBlockSize, static_cast<off_t>(offsets[i]));
                benchmark::DoNotOptimize(n);
                done_barrier.arrive_and_wait();
            }
        });
    }

    for (auto _ : state) {
        for (size_t i = 0; i < iodepth; ++i) {
            offsets[i] = rng.RandOffset(kTestFileSize, kBlockSize);
        }
        start_barrier.arrive_and_wait();
        done_barrier.arrive_and_wait();
    }

    running.store(false, std::memory_order_relaxed);
    start_barrier.arrive_and_wait();
    for (auto& t : threads) {
        t.join();
    }
    for (int fd : fds) {
        ::close(fd);
    }

    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(iodepth) *
                            static_cast<int64_t>(kBlockSize));
    state.counters["iodepth"] = static_cast<double>(iodepth);
}
BENCHMARK(BM_SyncMultiRandRead)->Arg(1)->Arg(4)->Arg(16)->Arg(64);

// ============================================================================
// Sync Multi-IODepth Random Write (N pre-created threads)
// ============================================================================

static void BM_SyncMultiRandWrite(benchmark::State& state) {
    const auto iodepth = static_cast<size_t>(state.range(0));
    EnsureSetup();
    const auto path = TestFilePath();

    std::vector<AlignedBuffer> bufs;
    bufs.reserve(iodepth);
    for (size_t i = 0; i < iodepth; ++i) {
        bufs.emplace_back(kBlockSize);
    }

    // Pre-open one fd per thread
    std::vector<int> fds(iodepth);
    for (size_t i = 0; i < iodepth; ++i) {
        fds[i] = ::open(path.c_str(), O_RDWR | O_DIRECT);
    }

    Rng rng(44444);
    std::vector<uint64_t> offsets(iodepth);
    std::atomic<bool> running{true};

    std::barrier start_barrier(static_cast<std::ptrdiff_t>(iodepth + 1));
    std::barrier done_barrier(static_cast<std::ptrdiff_t>(iodepth + 1));

    // Pre-create worker threads
    std::vector<std::thread> threads;
    threads.reserve(iodepth);
    for (size_t i = 0; i < iodepth; ++i) {
        threads.emplace_back([&, i]() {
            while (true) {
                start_barrier.arrive_and_wait();
                if (!running.load(std::memory_order_relaxed)) {
                    return;
                }
                auto n = ::pwrite(fds[i], bufs[i].ptr, kBlockSize, static_cast<off_t>(offsets[i]));
                benchmark::DoNotOptimize(n);
                done_barrier.arrive_and_wait();
            }
        });
    }

    for (auto _ : state) {
        for (size_t i = 0; i < iodepth; ++i) {
            offsets[i] = rng.RandOffset(kTestFileSize, kBlockSize);
        }
        start_barrier.arrive_and_wait();
        done_barrier.arrive_and_wait();
    }

    running.store(false, std::memory_order_relaxed);
    start_barrier.arrive_and_wait();
    for (auto& t : threads) {
        t.join();
    }
    for (int fd : fds) {
        ::close(fd);
    }

    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(iodepth) *
                            static_cast<int64_t>(kBlockSize));
    state.counters["iodepth"] = static_cast<double>(iodepth);
}
BENCHMARK(BM_SyncMultiRandWrite)->Arg(1)->Arg(4)->Arg(16)->Arg(64);

}  // namespace

#endif  // HOTCOCO_HAS_IOURING
