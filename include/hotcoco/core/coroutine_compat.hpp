// ============================================================================
// hotcoco/core/coroutine_compat.hpp - Compiler-Specific Coroutine Workarounds
// ============================================================================
//
// GCC has a long-standing bug (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=100897)
// where AddressSanitizer interferes with the tail-call optimization required for
// coroutine symmetric transfer. Without the tail call, each symmetric transfer
// adds a stack frame, eventually causing stack overflow in deep coroutine chains.
//
// This header provides a portable wrapper so that symmetric transfer works
// correctly under GCC+ASan. On unaffected compilers (Clang, or GCC without
// ASan) the wrapper compiles down to a plain handle return with zero overhead.
//
// HOW IT WORKS:
// -------------
// Normal symmetric transfer:
//   coroutine_handle<> await_suspend(...) { return target; }
//   // Compiler emits tail-call to target.resume() — no stack growth
//
// GCC+ASan broken behavior:
//   // ASan instrumentation prevents the tail-call, stack grows per transfer
//
// Our workaround (GCC+ASan only):
//   void await_suspend(...) { target.resume(); }
//   // Direct call — still grows the stack, but await_suspend(void) returns
//   // to the caller's resume() which then returns, so the stack unwinds
//   // between each pair of transfers. Not as optimal as true symmetric
//   // transfer, but prevents unbounded stack growth.
//
// ============================================================================

#pragma once

#include <coroutine>

namespace hotcoco {

// ============================================================================
// Detect GCC + AddressSanitizer
// ============================================================================
// GCC defines __SANITIZE_ADDRESS__ when -fsanitize=address is active.
// We only need the workaround for GCC; Clang handles symmetric transfer
// correctly even under ASan.
//
#if defined(__GNUG__) && !defined(__clang__) && defined(__SANITIZE_ADDRESS__)
#define HOTCOCO_ASAN_SYMMETRIC_TRANSFER_BROKEN 1
#else
#define HOTCOCO_ASAN_SYMMETRIC_TRANSFER_BROKEN 0
#endif

// ============================================================================
// SymmetricTransfer - Portable symmetric transfer helper
// ============================================================================
//
// Usage in await_suspend:
//
//   auto await_suspend(std::coroutine_handle<Promise> h) noexcept
//       -> SymmetricTransferResult {
//       auto target = h.promise().continuation_;
//       return SymmetricTransfer(target ? target : std::noop_coroutine());
//   }
//
// On normal builds, SymmetricTransferResult is coroutine_handle<> and
// SymmetricTransfer() is identity — the compiler sees a handle return and
// emits the tail-call.
//
// On GCC+ASan builds, SymmetricTransferResult is void and
// SymmetricTransfer() calls target.resume() directly.
//

#if HOTCOCO_ASAN_SYMMETRIC_TRANSFER_BROKEN

using SymmetricTransferResult = void;

inline void SymmetricTransfer(std::coroutine_handle<> target) noexcept {
    target.resume();
}

#else

using SymmetricTransferResult = std::coroutine_handle<>;

inline std::coroutine_handle<> SymmetricTransfer(std::coroutine_handle<> target) noexcept {
    return target;
}

#endif

}  // namespace hotcoco
