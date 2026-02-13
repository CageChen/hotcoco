// ============================================================================
// Error Code Tests
// ============================================================================

#include "hotcoco/core/error.hpp"

#include <gtest/gtest.h>
#include <string>

using namespace hotcoco;

// ============================================================================
// Basic Error Tests
// ============================================================================

TEST(ErrorTest, MakeErrorCode) {
    std::error_code ec = make_error_code(Errc::NoExecutor);
    EXPECT_TRUE(static_cast<bool>(ec));
    EXPECT_EQ(ec.value(), static_cast<int>(Errc::NoExecutor));
    EXPECT_EQ(std::string(ec.category().name()), "hotcoco");
}

TEST(ErrorTest, ErrorMessages) {
    EXPECT_EQ(make_error_code(Errc::NoExecutor).message(), "No current executor");
    EXPECT_EQ(make_error_code(Errc::InvalidArgument).message(), "Invalid argument");
    EXPECT_EQ(make_error_code(Errc::IoError).message(), "I/O error");
    EXPECT_EQ(make_error_code(Errc::SocketCreateFailed).message(), "Failed to create socket");
    EXPECT_EQ(make_error_code(Errc::ConnectFailed).message(), "Failed to connect");
    EXPECT_EQ(make_error_code(Errc::ResolveFailed).message(), "Failed to resolve host");
    EXPECT_EQ(make_error_code(Errc::BindFailed).message(), "Failed to bind");
    EXPECT_EQ(make_error_code(Errc::ListenFailed).message(), "Failed to listen");
    EXPECT_EQ(make_error_code(Errc::AcceptFailed).message(), "Failed to accept connection");
    EXPECT_EQ(make_error_code(Errc::SendFailed).message(), "Failed to send data");
    EXPECT_EQ(make_error_code(Errc::RecvFailed).message(), "Failed to receive data");
    EXPECT_EQ(make_error_code(Errc::NotConnected).message(), "Not connected");
    EXPECT_EQ(make_error_code(Errc::ListenerNotInitialized).message(), "Listener not initialized");
    EXPECT_EQ(make_error_code(Errc::RetryExhausted).message(), "All retry attempts exhausted");
    EXPECT_EQ(make_error_code(Errc::InvalidAddress).message(), "Invalid address");
    EXPECT_EQ(make_error_code(Errc::EventfdFailed).message(), "Failed to create eventfd");
    EXPECT_EQ(make_error_code(Errc::TimerfdFailed).message(), "Failed to create timerfd");
    EXPECT_EQ(make_error_code(Errc::IoUringInitFailed).message(), "Failed to initialize io_uring");
}

TEST(ErrorTest, CategorySingleton) {
    const auto& cat1 = HotcocoCategory();
    const auto& cat2 = HotcocoCategory();
    EXPECT_EQ(&cat1, &cat2);
}

TEST(ErrorTest, ImplicitConversionFromErrc) {
    // Errc is registered as is_error_code_enum, so implicit conversion works
    std::error_code ec = Errc::ConnectFailed;
    EXPECT_TRUE(static_cast<bool>(ec));
    EXPECT_EQ(ec.message(), "Failed to connect");
}

TEST(ErrorTest, ComparisonBetweenErrorCodes) {
    std::error_code ec1 = Errc::IoError;
    std::error_code ec2 = Errc::IoError;
    std::error_code ec3 = Errc::BindFailed;
    EXPECT_EQ(ec1, ec2);
    EXPECT_NE(ec1, ec3);
}

TEST(ErrorTest, ErrorAlias) {
    Error ec = Errc::NotConnected;
    EXPECT_TRUE(static_cast<bool>(ec));
    EXPECT_EQ(ec.message(), "Not connected");
}

TEST(ErrorTest, DefaultErrorCodeIsFalsy) {
    std::error_code ec;
    EXPECT_FALSE(static_cast<bool>(ec));
}

TEST(ErrorTest, UnknownErrorCodeMessage) {
    // Cast an out-of-range value to Errc to exercise the default branch
    std::error_code ec = make_error_code(static_cast<Errc>(9999));
    EXPECT_EQ(ec.message(), "Unknown hotcoco error");
}
