// ============================================================================
// tests/iouring_file_test.cpp - io_uring File Operations Tests
// ============================================================================

#ifdef HOTCOCO_HAS_IOURING

#include "hotcoco/io/iouring_file.hpp"

#include "hotcoco/core/result.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/io/iouring_executor.hpp"

#include <sys/stat.h>

#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace hotcoco;

namespace {

const std::string kTestDir = "/tmp/hotcoco_iouring_file_test";

class IoUringFileTest : public ::testing::Test {
   protected:
    void SetUp() override {
        std::filesystem::create_directories(kTestDir);
        auto r = IoUringExecutor::Create();
        EXPECT_TRUE(r.IsOk());
        executor_ = std::move(r).Value();
    }

    void TearDown() override {
        executor_.reset();
        std::filesystem::remove_all(kTestDir);
    }

    // Helper: schedule a Task<void> coroutine and run the executor until Stop
    void RunTask(Task<void> task) {
        executor_->Schedule(task.GetHandle());
        executor_->Run();
    }

    std::string TestPath(const std::string& name) { return kTestDir + "/" + name; }

    std::unique_ptr<IoUringExecutor> executor_;
};

// ============================================================================
// File::Open / Close
// ============================================================================

TEST_F(IoUringFileTest, OpenAndClose) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        auto result = co_await File::Open(TestPath("open_close.txt"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        EXPECT_TRUE(result.IsOk());
        auto file = std::move(result).Value();
        EXPECT_TRUE(file.IsOpen());
        EXPECT_GE(file.Fd(), 0);

        auto close_res = co_await file.Close();
        EXPECT_EQ(close_res, 0);
        EXPECT_FALSE(file.IsOpen());
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

TEST_F(IoUringFileTest, OpenNonExistent) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        auto result = co_await File::Open(TestPath("does_not_exist.txt"), O_RDONLY);
        EXPECT_TRUE(result.IsErr());
        auto ec = result.Error();
        EXPECT_EQ(ec, std::error_code(ENOENT, std::system_category()));
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

TEST_F(IoUringFileTest, DoubleClose) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        auto result = co_await File::Open(TestPath("double_close.txt"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        EXPECT_TRUE(result.IsOk());
        auto file = std::move(result).Value();

        auto r1 = co_await file.Close();
        EXPECT_EQ(r1, 0);

        auto r2 = co_await file.Close();
        EXPECT_EQ(r2, -EBADF);
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

// ============================================================================
// ReadAt / WriteAt
// ============================================================================

TEST_F(IoUringFileTest, WriteAndReadAt) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        auto result = co_await File::Open(TestPath("rw.txt"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        EXPECT_TRUE(result.IsOk());
        auto file = std::move(result).Value();

        const char* data = "Hello, io_uring!";
        auto len = static_cast<size_t>(std::strlen(data));

        auto wres = co_await file.WriteAt(data, len, 0);
        EXPECT_EQ(wres, static_cast<int32_t>(len));

        char buf[64] = {};
        auto rres = co_await file.ReadAt(buf, len, 0);
        EXPECT_EQ(rres, static_cast<int32_t>(len));
        EXPECT_EQ(std::string_view(buf, len), "Hello, io_uring!");

        co_await file.Close();
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

TEST_F(IoUringFileTest, WriteAtOffset) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        auto result = co_await File::Open(TestPath("offset.txt"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        EXPECT_TRUE(result.IsOk());
        auto file = std::move(result).Value();

        co_await file.WriteAt("AAAA", 4, 0);
        co_await file.WriteAt("BB", 2, 2);

        char buf[4] = {};
        co_await file.ReadAt(buf, 4, 0);
        EXPECT_EQ(std::string_view(buf, 4), "AABB");

        co_await file.Close();
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

TEST_F(IoUringFileTest, ReadPastEOF) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        auto result = co_await File::Open(TestPath("eof.txt"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        EXPECT_TRUE(result.IsOk());
        auto file = std::move(result).Value();

        co_await file.WriteAt("Hi", 2, 0);

        char buf[64] = {};
        auto rres = co_await file.ReadAt(buf, 64, 0);
        EXPECT_EQ(rres, 2);

        auto rres2 = co_await file.ReadAt(buf, 64, 100);
        EXPECT_EQ(rres2, 0);  // EOF

        co_await file.Close();
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

// ============================================================================
// Fsync
// ============================================================================

TEST_F(IoUringFileTest, Fsync) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        auto result = co_await File::Open(TestPath("fsync.txt"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        EXPECT_TRUE(result.IsOk());
        auto file = std::move(result).Value();

        co_await file.WriteAt("data", 4, 0);
        auto fres = co_await file.Fsync();
        EXPECT_EQ(fres, 0);

        co_await file.Close();
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

// ============================================================================
// ReadAll / WriteAll
// ============================================================================

TEST_F(IoUringFileTest, WriteAllAndReadAll) {
    bool ok = false;
    const std::string content = "The quick brown fox jumps over the lazy dog";

    auto task = [&]() -> Task<void> {
        // Write
        {
            auto result = co_await File::Open(TestPath("readall.txt"), O_RDWR | O_CREAT | O_TRUNC, 0644);
            EXPECT_TRUE(result.IsOk());
            auto file = std::move(result).Value();

            auto wres = co_await file.WriteAll(content.data(), content.size());
            EXPECT_TRUE(wres.IsOk());
            EXPECT_EQ(wres.Value(), content.size());

            co_await file.Close();
        }

        // Read
        {
            auto result = co_await File::Open(TestPath("readall.txt"), O_RDONLY);
            EXPECT_TRUE(result.IsOk());
            auto file = std::move(result).Value();

            auto rres = co_await file.ReadAll();
            EXPECT_TRUE(rres.IsOk());
            auto data = std::move(rres).Value();
            EXPECT_EQ(std::string_view(data.data(), data.size()), content);

            co_await file.Close();
        }

        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

TEST_F(IoUringFileTest, ReadAllEmptyFile) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        auto result = co_await File::Open(TestPath("empty.txt"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        EXPECT_TRUE(result.IsOk());
        auto file = std::move(result).Value();

        auto rres = co_await file.ReadAll();
        EXPECT_TRUE(rres.IsOk());
        EXPECT_TRUE(rres.Value().empty());

        co_await file.Close();
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

// ============================================================================
// Move Semantics
// ============================================================================

TEST_F(IoUringFileTest, MoveConstruct) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        auto result = co_await File::Open(TestPath("move.txt"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        EXPECT_TRUE(result.IsOk());
        auto file1 = std::move(result).Value();

        int fd = file1.Fd();
        File file2(std::move(file1));

        EXPECT_FALSE(file1.IsOpen());  // NOLINT source has been moved
        EXPECT_TRUE(file2.IsOpen());
        EXPECT_EQ(file2.Fd(), fd);

        co_await file2.Close();
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

TEST_F(IoUringFileTest, MoveAssign) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        auto r1 = co_await File::Open(TestPath("move_a.txt"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        auto r2 = co_await File::Open(TestPath("move_b.txt"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        EXPECT_TRUE(r1.IsOk());
        EXPECT_TRUE(r2.IsOk());
        auto file1 = std::move(r1).Value();
        auto file2 = std::move(r2).Value();

        int fd2 = file2.Fd();
        file1 = std::move(file2);

        EXPECT_EQ(file1.Fd(), fd2);
        EXPECT_FALSE(file2.IsOpen());  // NOLINT source has been moved

        co_await file1.Close();
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

// ============================================================================
// Destructor RAII
// ============================================================================

TEST_F(IoUringFileTest, DestructorClosesFile) {
    int captured_fd = -1;

    auto task = [&]() -> Task<void> {
        auto result = co_await File::Open(TestPath("raii.txt"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        EXPECT_TRUE(result.IsOk());
        auto file = std::move(result).Value();
        captured_fd = file.Fd();
        // file goes out of scope here — destructor should close fd
        executor_->Stop();
    };

    RunTask(task());

    // Verify the fd is closed by trying to fstat it
    ASSERT_GE(captured_fd, 0);
    struct stat st{};
    EXPECT_EQ(fstat(captured_fd, &st), -1);
    EXPECT_EQ(errno, EBADF);
}

// ============================================================================
// Utility Free Functions: IoUringUnlink
// ============================================================================

TEST_F(IoUringFileTest, Unlink) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        auto result = co_await File::Open(TestPath("to_delete.txt"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        EXPECT_TRUE(result.IsOk());
        co_await std::move(result).Value().Close();

        auto res = co_await IoUringUnlink(TestPath("to_delete.txt"));
        EXPECT_EQ(res, 0);

        auto res2 = co_await File::Open(TestPath("to_delete.txt"), O_RDONLY);
        EXPECT_TRUE(res2.IsErr());
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

TEST_F(IoUringFileTest, UnlinkNonExistent) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        auto res = co_await IoUringUnlink(TestPath("no_such_file.txt"));
        EXPECT_EQ(res, -ENOENT);
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

// ============================================================================
// Utility Free Functions: IoUringMkdir
// ============================================================================

TEST_F(IoUringFileTest, Mkdir) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        auto res = co_await IoUringMkdir(TestPath("new_dir"), 0755);
        EXPECT_EQ(res, 0);

        struct stat st{};
        EXPECT_EQ(stat(TestPath("new_dir").c_str(), &st), 0);
        EXPECT_TRUE(S_ISDIR(st.st_mode));
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

TEST_F(IoUringFileTest, MkdirAlreadyExists) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        co_await IoUringMkdir(TestPath("existing_dir"), 0755);
        auto res = co_await IoUringMkdir(TestPath("existing_dir"), 0755);
        EXPECT_EQ(res, -EEXIST);
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

// ============================================================================
// Utility Free Functions: IoUringStat
// ============================================================================

TEST_F(IoUringFileTest, Stat) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        auto result = co_await File::Open(TestPath("stat_me.txt"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        EXPECT_TRUE(result.IsOk());
        auto file = std::move(result).Value();
        co_await file.WriteAt("0123456789", 10, 0);
        co_await file.Close();

        auto stat_result = co_await IoUringStat(TestPath("stat_me.txt"));
        EXPECT_TRUE(stat_result.IsOk());
        auto stx = stat_result.Value();
        EXPECT_EQ(stx.stx_size, 10u);
        EXPECT_TRUE(S_ISREG(stx.stx_mode));
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

TEST_F(IoUringFileTest, StatNonExistent) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        auto stat_result = co_await IoUringStat(TestPath("ghost.txt"));
        EXPECT_TRUE(stat_result.IsErr());
        EXPECT_EQ(stat_result.Error(), std::error_code(ENOENT, std::system_category()));
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

// ============================================================================
// Large File I/O (cross page boundary)
// ============================================================================

TEST_F(IoUringFileTest, LargeWriteAndReadAll) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        const size_t size = 65536;
        std::vector<char> write_data(size);
        for (size_t i = 0; i < size; ++i) {
            write_data[i] = static_cast<char>('A' + (i % 26));
        }

        auto result = co_await File::Open(TestPath("large.bin"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        EXPECT_TRUE(result.IsOk());
        auto file = std::move(result).Value();

        auto wres = co_await file.WriteAll(write_data.data(), write_data.size());
        EXPECT_TRUE(wres.IsOk());
        EXPECT_EQ(wres.Value(), size);

        auto rres = co_await file.ReadAll();
        EXPECT_TRUE(rres.IsOk());
        auto read_data = std::move(rres).Value();
        EXPECT_EQ(read_data.size(), size);
        EXPECT_EQ(read_data, write_data);

        co_await file.Close();
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

// ============================================================================
// Operations on closed file
// ============================================================================

TEST_F(IoUringFileTest, OperationsOnClosedFile) {
    bool ok = false;
    auto task = [&]() -> Task<void> {
        auto result = co_await File::Open(TestPath("closed_ops.txt"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        EXPECT_TRUE(result.IsOk());
        auto file = std::move(result).Value();
        co_await file.Close();

        char buf[4] = {};
        EXPECT_EQ(co_await file.ReadAt(buf, 4, 0), -EBADF);
        EXPECT_EQ(co_await file.WriteAt("hi", 2, 0), -EBADF);
        EXPECT_EQ(co_await file.Fsync(), -EBADF);
        ok = true;
        executor_->Stop();
    };

    RunTask(task());
    EXPECT_TRUE(ok);
}

}  // namespace

#else  // !HOTCOCO_HAS_IOURING

#include <gtest/gtest.h>

TEST(IoUringFileTest, NotAvailable) {
    GTEST_SKIP() << "io_uring not available";
}

#endif  // HOTCOCO_HAS_IOURING
