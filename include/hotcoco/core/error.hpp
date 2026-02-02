// ============================================================================
// hotcoco/core/error.hpp - Error Codes for hotcoco
// ============================================================================
//
// Defines a std::error_code-based error infrastructure for the library.
// All errors are represented as std::error_code values using the hotcoco
// error category.
//
// USAGE:
// ------
//   std::error_code ec = make_error_code(Errc::NoExecutor);
//   if (ec) { /* handle error */ }
//
// ============================================================================

#pragma once

#include <system_error>

namespace hotcoco {

enum class Errc {
    NoExecutor = 1,
    InvalidArgument,
    IoError,
    SocketCreateFailed,
    ConnectFailed,
    ResolveFailed,
    BindFailed,
    ListenFailed,
    AcceptFailed,
    SendFailed,
    RecvFailed,
    NotConnected,
    ListenerNotInitialized,
    RetryExhausted,
    InvalidAddress,
    EventfdFailed,
    TimerfdFailed,
    IoUringInitFailed,
};

const std::error_category& HotcocoCategory() noexcept;

std::error_code make_error_code(Errc e) noexcept;

// Convenient alias used throughout the library
using Error = std::error_code;

}  // namespace hotcoco

// Register with std::error_code
namespace std {
template <>
struct is_error_code_enum<hotcoco::Errc> : true_type {};
}  // namespace std
