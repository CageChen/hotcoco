// ============================================================================
// hotcoco/core/check.hpp - Lightweight Runtime Assertions
// ============================================================================
//
// HOTCOCO_CHECK(cond, msg) is an always-on assertion that works in both
// Debug and Release builds. Unlike assert(), it is never compiled out.
//
// On failure it prints the condition, message, and source location to stderr,
// then calls std::abort(). The failure branch is marked [[unlikely]] so the
// compiler keeps it off the hot path.
//
// Use HOTCOCO_CHECK for precondition violations that represent programming
// errors (e.g. double-await, invalid constructor arguments). For conditions
// that are debug-only diagnostics, continue using assert().
//
// ============================================================================

#pragma once

#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace hotcoco::detail {

// Print an unsigned integer to a buffer, return pointer to start of digits.
// Buffer must be at least 10 bytes (max digits for uint32_t).
inline char* UintToStr(unsigned int value, char* buf_end) {
    char* p = buf_end;
    do {
        *--p = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    return p;
}

[[noreturn]] inline void CheckFail(const char* cond_str, const char* msg, const std::source_location& loc) {
    // Build output without varargs to satisfy cppcoreguidelines-pro-type-vararg
    char line_buf[12];
    char* line_end = line_buf + sizeof(line_buf);
    char* line_str = UintToStr(loc.line(), line_end);

    std::fputs("HOTCOCO_CHECK(", stderr);
    std::fputs(cond_str, stderr);
    std::fputs(") failed: ", stderr);
    std::fputs(msg, stderr);
    std::fputs("\n  in ", stderr);
    std::fputs(loc.function_name(), stderr);
    std::fputs(" (", stderr);
    std::fputs(loc.file_name(), stderr);
    std::fputs(":", stderr);
    std::fwrite(line_str, 1, static_cast<size_t>(line_end - line_str), stderr);
    std::fputs(")\n", stderr);
    std::abort();
}

}  // namespace hotcoco::detail

#define HOTCOCO_CHECK(cond, msg)                                                       \
    do {                                                                               \
        if (!(cond)) [[unlikely]] {                                                    \
            ::hotcoco::detail::CheckFail(#cond, msg, std::source_location::current()); \
        }                                                                              \
    } while (0)
