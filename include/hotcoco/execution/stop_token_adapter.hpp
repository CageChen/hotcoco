// ============================================================================
// hotcoco/execution/stop_token_adapter.hpp - stop_token ↔ CancellationToken
// ============================================================================
//
// Bidirectional bridge between std::stop_token (P2300 cancellation) and
// hotcoco::CancellationToken (hotcoco cancellation).
//
// USAGE:
// ------
//   // std::stop_token → CancellationToken
//   std::stop_source ss;
//   auto [token, bridge] = hotcoco::execution::FromStopToken(ss.get_token());
//   ss.request_stop();
//   assert(token.IsCancelled());
//
//   // CancellationToken → std::stop_source linkage
//   CancellationSource source;
//   std::stop_source ss2;
//   auto link = hotcoco::execution::LinkCancellation(source.GetToken(), ss2);
//   source.Cancel();
//   assert(ss2.stop_requested());
//
// ============================================================================

#pragma once

#include "hotcoco/core/cancellation.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

namespace hotcoco::execution {

// ============================================================================
// FromStopToken - std::stop_token → CancellationToken
// ============================================================================
// Creates a CancellationToken that is triggered when the std::stop_token
// is signaled. The bridge holds a std::stop_callback that calls Cancel()
// on the underlying CancellationSource.

class StopTokenBridge {
   public:
    explicit StopTokenBridge(std::stop_token st) {
        if (st.stop_possible()) {
            callback_.emplace(std::move(st), [this] { source_.Cancel(); });
        }
    }

    ~StopTokenBridge() = default;

    StopTokenBridge(const StopTokenBridge&) = delete;
    StopTokenBridge& operator=(const StopTokenBridge&) = delete;

    [[nodiscard]] CancellationToken GetToken() const { return source_.GetToken(); }

   private:
    CancellationSource source_;
    std::optional<std::stop_callback<std::function<void()>>> callback_;
};

struct StopTokenBridgeHandle {
    CancellationToken token;
    std::shared_ptr<StopTokenBridge> bridge;
};

inline StopTokenBridgeHandle FromStopToken(std::stop_token st) {
    auto bridge = std::make_shared<StopTokenBridge>(std::move(st));
    auto token = bridge->GetToken();
    return {std::move(token), std::move(bridge)};
}

// ============================================================================
// LinkCancellation - CancellationToken → std::stop_source
// ============================================================================
// When the CancellationToken is cancelled, the linked std::stop_source
// is also signaled. Returns an opaque handle that unregisters the callback
// on destruction. The caller must keep the handle alive.

class CancellationLink {
   public:
    // Takes stop_source by value — std::stop_source has shared-ownership
    // semantics so copies are safe and avoid dangling reference risk.
    CancellationLink(CancellationToken token, std::stop_source source) : token_(std::move(token)) {
        handle_ = token_.OnCancel([s = std::move(source)]() mutable { s.request_stop(); });
    }

    ~CancellationLink() { token_.Unregister(handle_); }

    CancellationLink(const CancellationLink&) = delete;
    CancellationLink& operator=(const CancellationLink&) = delete;
    CancellationLink(CancellationLink&&) = delete;
    CancellationLink& operator=(CancellationLink&&) = delete;

   private:
    CancellationToken token_;
    size_t handle_ = 0;
};

inline std::unique_ptr<CancellationLink> LinkCancellation(CancellationToken token, std::stop_source source) {
    return std::make_unique<CancellationLink>(std::move(token), std::move(source));
}

}  // namespace hotcoco::execution
