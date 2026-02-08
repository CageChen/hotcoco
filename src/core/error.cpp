// ============================================================================
// hotcoco/core/error.cpp - Error Category Implementation
// ============================================================================

#include "hotcoco/core/error.hpp"

#include <string>

namespace hotcoco {

namespace {

class HotcocoCategoryImpl : public std::error_category {
   public:
    const char* name() const noexcept override { return "hotcoco"; }

    std::string message(int ev) const override {
        switch (static_cast<Errc>(ev)) {
            case Errc::NoExecutor:
                return "No current executor";
            case Errc::InvalidArgument:
                return "Invalid argument";
            case Errc::IoError:
                return "I/O error";
            case Errc::SocketCreateFailed:
                return "Failed to create socket";
            case Errc::ConnectFailed:
                return "Failed to connect";
            case Errc::ResolveFailed:
                return "Failed to resolve host";
            case Errc::BindFailed:
                return "Failed to bind";
            case Errc::ListenFailed:
                return "Failed to listen";
            case Errc::AcceptFailed:
                return "Failed to accept connection";
            case Errc::SendFailed:
                return "Failed to send data";
            case Errc::RecvFailed:
                return "Failed to receive data";
            case Errc::NotConnected:
                return "Not connected";
            case Errc::ListenerNotInitialized:
                return "Listener not initialized";
            case Errc::RetryExhausted:
                return "All retry attempts exhausted";
            case Errc::InvalidAddress:
                return "Invalid address";
            case Errc::EventfdFailed:
                return "Failed to create eventfd";
            case Errc::TimerfdFailed:
                return "Failed to create timerfd";
            case Errc::IoUringInitFailed:
                return "Failed to initialize io_uring";
            default:
                return "Unknown hotcoco error";
        }
    }
};

}  // namespace

const std::error_category& HotcocoCategory() noexcept {
    static const HotcocoCategoryImpl instance;
    return instance;
}

std::error_code make_error_code(Errc e) noexcept {
    return {static_cast<int>(e), HotcocoCategory()};
}

}  // namespace hotcoco
