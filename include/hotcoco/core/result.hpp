// ============================================================================
// hotcoco/core/result.hpp - Result Type for Error Handling
// ============================================================================
//
// Result<T, E> is a discriminated union that holds either a success value (T)
// or an error value (E). It provides a type-safe, exception-free way to
// handle errors in coroutines.
//
// DESIGN PHILOSOPHY:
// ------------------
// 1. EXPLICIT ERRORS: Errors are part of the return type, not hidden exceptions
// 2. FORCE HANDLING: Must check before accessing value
// 3. COMPOSABLE: Chain operations with Map(), AndThen(), OrElse()
//
// USAGE:
// ------
//   Result<int, std::string> Divide(int a, int b) {
//       if (b == 0) return Err("division by zero");
//       return Ok(a / b);
//   }
//
//   auto result = Divide(10, 2);
//   if (result.IsOk()) {
//       std::cout << result.Value() << std::endl;  // 5
//   }
//
// WITH COROUTINES:
// ----------------
//   Task<Result<Data, Error>> FetchData(std::string url);
//
//   Task<void> Process() {
//       auto result = co_await FetchData("http://example.com");
//       if (result.IsErr()) {
//           std::cerr << result.Error().message << std::endl;
//           co_return;
//       }
//       UseData(result.Value());
//   }
//
// ============================================================================

#pragma once

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace hotcoco {

// Forward declarations
template <typename T, typename E>
class Result;

// ============================================================================
// Ok and Err Tag Types
// ============================================================================
// These allow type deduction in factory functions

template <typename T>
struct OkTag {
    T value;

    template <typename U>
    explicit OkTag(U&& v) : value(std::forward<U>(v)) {}
};

template <typename E>
struct ErrTag {
    E error;

    template <typename U>
    explicit ErrTag(U&& e) : error(std::forward<U>(e)) {}
};

// Factory functions
template <typename T>
OkTag<std::decay_t<T>> Ok(T&& value) {
    return OkTag<std::decay_t<T>>(std::forward<T>(value));
}

template <typename E>
ErrTag<std::decay_t<E>> Err(E&& error) {
    return ErrTag<std::decay_t<E>>(std::forward<E>(error));
}

// Unit type for Result<void, E>
struct Unit {};

inline OkTag<Unit> Ok() {
    return OkTag<Unit>(Unit{});
}

// ============================================================================
// Result<T, E> - Success or Error
// ============================================================================
template <typename T, typename E>
class Result {
   public:
    // ========================================================================
    // Construction
    // ========================================================================

    // Construct from OkTag
    template <typename U>
    Result(OkTag<U>&& ok) : data_(std::in_place_index<0>, std::move(ok.value)) {}

    // Construct from ErrTag
    template <typename U>
    Result(ErrTag<U>&& err) : data_(std::in_place_index<1>, std::move(err.error)) {}

    // Copy/Move
    Result(const Result&) = default;
    Result(Result&&) = default;
    Result& operator=(const Result&) = default;
    Result& operator=(Result&&) = default;

    // ========================================================================
    // Observers
    // ========================================================================

    bool IsOk() const noexcept { return data_.index() == 0; }
    bool IsErr() const noexcept { return data_.index() == 1; }

    explicit operator bool() const noexcept { return IsOk(); }

    // ========================================================================
    // Accessors
    // ========================================================================

    // Get value (undefined behavior if IsErr())
    T& Value() & { return std::get<0>(data_); }
    const T& Value() const& { return std::get<0>(data_); }
    T&& Value() && { return std::get<0>(std::move(data_)); }

    // Get error (undefined behavior if IsOk())
    E& Error() & { return std::get<1>(data_); }
    const E& Error() const& { return std::get<1>(data_); }
    E&& Error() && { return std::get<1>(std::move(data_)); }

    // Safe accessors
    std::optional<T> ValueOr() const {
        if (IsOk()) return std::get<0>(data_);
        return std::nullopt;
    }

    T ValueOr(T default_value) const {
        if (IsOk()) return std::get<0>(data_);
        return default_value;
    }

    // ========================================================================
    // Combinators
    // ========================================================================

    // Map: Transform success value
    template <typename F>
    auto Map(F&& func) const& -> Result<std::invoke_result_t<F, const T&>, E> {
        if (IsOk()) {
            return Ok(func(Value()));
        }
        return Err(Error());
    }

    template <typename F>
    auto Map(F&& func) && -> Result<std::invoke_result_t<F, T&&>, E> {
        if (IsOk()) {
            return Ok(func(std::move(*this).Value()));
        }
        return Err(std::move(*this).Error());
    }

    // MapErr: Transform error value
    template <typename F>
    auto MapErr(F&& func) const& -> Result<T, std::invoke_result_t<F, const E&>> {
        if (IsErr()) {
            return Err(func(Error()));
        }
        return Ok(Value());
    }

    // AndThen: Chain operations that return Result
    template <typename F>
    auto AndThen(F&& func) && -> std::invoke_result_t<F, T&&> {
        if (IsOk()) {
            return func(std::move(*this).Value());
        }
        return Err(std::move(*this).Error());
    }

    // OrElse: Recover from error
    template <typename F>
    auto OrElse(F&& func) && -> std::invoke_result_t<F, E&&> {
        if (IsErr()) {
            return func(std::move(*this).Error());
        }
        return Ok(std::move(*this).Value());
    }

   private:
    std::variant<T, E> data_;
};

// ============================================================================
// Result<void, E> Specialization
// ============================================================================
// For operations that succeed without a value

template <typename E>
class Result<void, E> {
   public:
    Result(OkTag<Unit>&&) : error_(std::nullopt) {}

    template <typename U>
    Result(ErrTag<U>&& err) : error_(std::move(err.error)) {}

    bool IsOk() const noexcept { return !error_.has_value(); }
    bool IsErr() const noexcept { return error_.has_value(); }

    explicit operator bool() const noexcept { return IsOk(); }

    E& Error() & { return *error_; }
    const E& Error() const& { return *error_; }
    E&& Error() && { return std::move(*error_); }

   private:
    std::optional<E> error_;
};

// ============================================================================
// Comparison Operators
// ============================================================================

template <typename T, typename E>
bool operator==(const Result<T, E>& lhs, const Result<T, E>& rhs) {
    if (lhs.IsOk() != rhs.IsOk()) return false;
    if (lhs.IsOk()) return lhs.Value() == rhs.Value();
    return lhs.Error() == rhs.Error();
}

template <typename T, typename E>
bool operator!=(const Result<T, E>& lhs, const Result<T, E>& rhs) {
    return !(lhs == rhs);
}

}  // namespace hotcoco
