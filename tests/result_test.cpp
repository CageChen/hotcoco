// ============================================================================
// Result Type Tests
// ============================================================================

#include <gtest/gtest.h>

#include <string>

#include "hotcoco/core/result.hpp"
#include "hotcoco/core/task.hpp"
#include "hotcoco/sync/sync_wait.hpp"

using namespace hotcoco;

// ============================================================================
// Basic Result Tests
// ============================================================================

TEST(ResultTest, OkConstruction) {
    Result<int, std::string> result = Ok(42);
    
    EXPECT_TRUE(result.IsOk());
    EXPECT_FALSE(result.IsErr());
    EXPECT_EQ(result.Value(), 42);
}

TEST(ResultTest, ErrConstruction) {
    Result<int, std::string> result = Err(std::string("error"));
    
    EXPECT_FALSE(result.IsOk());
    EXPECT_TRUE(result.IsErr());
    EXPECT_EQ(result.Error(), "error");
}

TEST(ResultTest, BoolConversion) {
    Result<int, std::string> ok = Ok(42);
    Result<int, std::string> err = Err(std::string("error"));
    
    EXPECT_TRUE(static_cast<bool>(ok));
    EXPECT_FALSE(static_cast<bool>(err));
    
    if (ok) {
        EXPECT_EQ(ok.Value(), 42);
    } else {
        FAIL() << "Expected Ok result";
    }
}

TEST(ResultTest, ValueOr) {
    Result<int, std::string> ok = Ok(42);
    Result<int, std::string> err = Err(std::string("error"));
    
    EXPECT_EQ(ok.ValueOr(0), 42);
    EXPECT_EQ(err.ValueOr(0), 0);
}

TEST(ResultTest, ValueOrOptional) {
    Result<int, std::string> ok = Ok(42);
    Result<int, std::string> err = Err(std::string("error"));
    
    auto opt_ok = ok.ValueOr();
    auto opt_err = err.ValueOr();
    
    EXPECT_TRUE(opt_ok.has_value());
    EXPECT_EQ(*opt_ok, 42);
    EXPECT_FALSE(opt_err.has_value());
}

// ============================================================================
// Combinator Tests
// ============================================================================

TEST(ResultTest, Map) {
    Result<int, std::string> ok = Ok(21);
    Result<int, std::string> err = Err(std::string("error"));
    
    auto doubled_ok = ok.Map([](int x) { return x * 2; });
    auto doubled_err = err.Map([](int x) { return x * 2; });
    
    EXPECT_TRUE(doubled_ok.IsOk());
    EXPECT_EQ(doubled_ok.Value(), 42);
    
    EXPECT_TRUE(doubled_err.IsErr());
    EXPECT_EQ(doubled_err.Error(), "error");
}

TEST(ResultTest, MapToString) {
    Result<int, std::string> ok = Ok(42);
    
    auto str_result = ok.Map([](int x) { return std::to_string(x); });
    
    EXPECT_TRUE(str_result.IsOk());
    EXPECT_EQ(str_result.Value(), "42");
}

TEST(ResultTest, MapErr) {
    Result<int, int> err = Err(1);
    
    auto mapped = err.MapErr([](int e) { return e * 10; });
    
    EXPECT_TRUE(mapped.IsErr());
    EXPECT_EQ(mapped.Error(), 10);
}

TEST(ResultTest, AndThen) {
    auto parse = [](const std::string& s) -> Result<int, std::string> {
        try {
            return Ok(std::stoi(s));
        } catch (...) {
            return Err(std::string("parse error"));
        }
    };
    
    Result<std::string, std::string> ok = Ok(std::string("42"));
    Result<std::string, std::string> bad = Ok(std::string("not a number"));
    Result<std::string, std::string> err = Err(std::string("initial error"));
    
    auto result1 = std::move(ok).AndThen(parse);
    auto result2 = std::move(bad).AndThen(parse);
    auto result3 = std::move(err).AndThen(parse);
    
    EXPECT_TRUE(result1.IsOk());
    EXPECT_EQ(result1.Value(), 42);
    
    EXPECT_TRUE(result2.IsErr());
    EXPECT_EQ(result2.Error(), "parse error");
    
    EXPECT_TRUE(result3.IsErr());
    EXPECT_EQ(result3.Error(), "initial error");
}

// ============================================================================
// Void Result Tests
// ============================================================================

TEST(ResultTest, VoidOk) {
    Result<void, std::string> result = Ok();
    
    EXPECT_TRUE(result.IsOk());
    EXPECT_FALSE(result.IsErr());
}

TEST(ResultTest, VoidErr) {
    Result<void, std::string> result = Err(std::string("error"));
    
    EXPECT_FALSE(result.IsOk());
    EXPECT_TRUE(result.IsErr());
    EXPECT_EQ(result.Error(), "error");
}

// ============================================================================
// Result with Coroutines
// ============================================================================

Task<Result<int, std::string>> Divide(int a, int b) {
    if (b == 0) {
        co_return Err(std::string("division by zero"));
    }
    co_return Ok(a / b);
}

TEST(ResultTest, WithCoroutines) {
    auto test = []() -> Task<void> {
        auto result1 = co_await Divide(10, 2);
        EXPECT_TRUE(result1.IsOk());
        EXPECT_EQ(result1.Value(), 5);
        
        auto result2 = co_await Divide(10, 0);
        EXPECT_TRUE(result2.IsErr());
        EXPECT_EQ(result2.Error(), "division by zero");
    };
    
    SyncWait(test());
}

TEST(ResultTest, ChainedCoroutines) {
    auto compute = [](int x) -> Task<Result<int, std::string>> {
        auto result = co_await Divide(100, x);
        if (result.IsErr()) {
            co_return std::move(result);
        }
        co_return Ok(result.Value() * 2);
    };
    
    auto test = [&compute]() -> Task<void> {
        // Test successful computation
        auto result = co_await compute(5);
        EXPECT_TRUE(result.IsOk());
        EXPECT_EQ(result.Value(), 40);  // (100/5) * 2 = 40
        
        // Test error propagation
        auto err_result = co_await compute(0);
        EXPECT_TRUE(err_result.IsErr());
        EXPECT_EQ(err_result.Error(), "division by zero");
    };
    
    SyncWait(test());
}

// ============================================================================
// Comparison Tests
// ============================================================================

TEST(ResultTest, Equality) {
    Result<int, std::string> ok1 = Ok(42);
    Result<int, std::string> ok2 = Ok(42);
    Result<int, std::string> ok3 = Ok(0);
    Result<int, std::string> err1 = Err(std::string("a"));
    Result<int, std::string> err2 = Err(std::string("a"));
    Result<int, std::string> err3 = Err(std::string("b"));
    
    EXPECT_EQ(ok1, ok2);
    EXPECT_NE(ok1, ok3);
    EXPECT_NE(ok1, err1);
    EXPECT_EQ(err1, err2);
    EXPECT_NE(err1, err3);
}

// ============================================================================
// Additional Combinators and Edge Cases
// ============================================================================

TEST(ResultTest, MoveConstruction) {
    Result<std::unique_ptr<int>, std::string> ok = Ok(std::make_unique<int>(42));
    EXPECT_TRUE(ok.IsOk());

    auto moved = std::move(ok);
    EXPECT_TRUE(moved.IsOk());
    EXPECT_EQ(*moved.Value(), 42);
}

TEST(ResultTest, OrElse) {
    Result<int, int> err = Err(1);
    auto recovered = std::move(err).OrElse([](int e) -> Result<int, int> {
        return Ok(e * 100);
    });
    EXPECT_TRUE(recovered.IsOk());
    EXPECT_EQ(recovered.Value(), 100);
}

TEST(ResultTest, OrElseOkPassesThrough) {
    Result<int, int> ok = Ok(42);
    auto same = std::move(ok).OrElse([](int) -> Result<int, int> {
        return Ok(0);
    });
    EXPECT_TRUE(same.IsOk());
    EXPECT_EQ(same.Value(), 42);
}

TEST(ResultTest, MapErrOnOk) {
    Result<int, int> ok = Ok(42);
    auto mapped = ok.MapErr([](int e) { return e * 10; });
    EXPECT_TRUE(mapped.IsOk());
    EXPECT_EQ(mapped.Value(), 42);
}

TEST(ResultTest, MapWithRvalueRef) {
    Result<std::string, int> ok = Ok(std::string("hello"));
    auto mapped = std::move(ok).Map([](std::string&& s) {
        return s + " world";
    });
    EXPECT_TRUE(mapped.IsOk());
    EXPECT_EQ(mapped.Value(), "hello world");
}

TEST(ResultTest, VoidOkBoolConversion) {
    Result<void, std::string> ok = Ok();
    EXPECT_TRUE(static_cast<bool>(ok));

    Result<void, std::string> err = Err(std::string("fail"));
    EXPECT_FALSE(static_cast<bool>(err));
}

TEST(ResultTest, VoidErrAccess) {
    Result<void, int> err = Err(42);
    EXPECT_EQ(err.Error(), 42);
    EXPECT_EQ(std::move(err).Error(), 42);
}

TEST(ResultTest, AndThenChain) {
    auto step1 = [](int x) -> Result<std::string, std::string> {
        if (x < 0) return Err(std::string("negative"));
        return Ok(std::to_string(x));
    };

    auto step2 = [](std::string s) -> Result<int, std::string> {
        return Ok(static_cast<int>(s.size()));
    };

    Result<int, std::string> ok = Ok(12345);
    auto r = std::move(ok).AndThen(step1).AndThen(step2);
    EXPECT_TRUE(r.IsOk());
    EXPECT_EQ(r.Value(), 5);  // "12345" has length 5
}

TEST(ResultTest, AndThenShortCircuits) {
    Result<int, std::string> err = Err(std::string("original"));
    bool called = false;
    auto r = std::move(err).AndThen([&](int) -> Result<int, std::string> {
        called = true;
        return Ok(99);
    });
    EXPECT_FALSE(called);
    EXPECT_TRUE(r.IsErr());
    EXPECT_EQ(r.Error(), "original");
}
