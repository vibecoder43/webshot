#include <memory>
#include <optional>

#include <userver/utest/utest.hpp>

#include "try.hpp"

namespace {

enum class TestError {
    kAlpha,
    kBeta,
};

struct MoveOnly final {
    explicit MoveOnly(int v) : value(std::make_unique<int>(v)) {}

    MoveOnly(const MoveOnly &) = delete;
    MoveOnly(MoveOnly &&) noexcept = default;
    MoveOnly &operator=(const MoveOnly &) = delete;
    MoveOnly &operator=(MoveOnly &&) noexcept = default;

    std::unique_ptr<int> value;
};

[[nodiscard]] ws::Expected<int, TestError> MakeValue(bool ok)
{
    if (!ok)
        return ws::Unex(TestError::kAlpha);
    return 21;
}

[[nodiscard]] ws::Expected<void, TestError> MakeVoid(bool ok)
{
    if (!ok)
        return ws::Unex(TestError::kBeta);
    return {};
}

[[nodiscard]] std::optional<int> MaybeValue(bool ok)
{
    if (!ok)
        return {};
    return 7;
}

[[nodiscard]] std::optional<std::unique_ptr<int>> MaybePointer(bool ok)
{
    if (!ok)
        return {};
    return std::make_unique<int>(13);
}

[[nodiscard]] ws::Expected<MoveOnly, TestError> MakeMoveOnly(bool ok)
{
    if (!ok)
        return ws::Unex(TestError::kBeta);
    return {33};
}

[[nodiscard]] ws::Expected<int, TestError> DoubleExpected(bool ok)
{
    return TRY(MakeValue(ok)) * 2;
}

[[nodiscard]] ws::Expected<int, TestError> PropagateVoid(bool ok)
{
    TRY(MakeVoid(ok));
    return 9;
}

[[nodiscard]] std::optional<int> DoubleOptional(bool ok) { return TRY(MaybeValue(ok)) * 2; }

[[nodiscard]] std::optional<int> OptionalFromExpected(bool ok) { return TRY(MakeValue(ok)) * 2; }

[[nodiscard]] std::optional<int> OptionalFromExpectedVoid(bool ok)
{
    TRY(MakeVoid(ok));
    return 9;
}

[[nodiscard]] std::optional<int> LvalueOptional(bool ok)
{
    auto value = MaybeValue(ok);
    return TRY(value) + 1;
}

[[nodiscard]] std::optional<int> LvalueOptionalFromExpected(bool ok)
{
    auto value = MakeValue(ok);
    return TRY(value) + 1;
}

[[nodiscard]] ws::Expected<int, TestError> LvalueExpected(bool ok)
{
    auto value = MakeValue(ok);
    return TRY(value) + 1;
}

[[nodiscard]] ws::Expected<int, TestError> TwoTrys(bool left_ok, bool right_ok)
{
    return TRY(MakeValue(left_ok)) + TRY(MakeValue(right_ok));
}

[[nodiscard]] ws::Expected<int, TestError> SingleEvaluation(int &calls, bool ok)
{
    return TRY(([&]() -> ws::Expected<int, TestError> {
        calls++;
        if (!ok)
            return ws::Unex(TestError::kBeta);
        return 5;
    })());
}

[[nodiscard]] ws::Expected<int, TestError> UnwrapMoveOnly(bool ok)
{
    auto value = TRY(MakeMoveOnly(ok));
    return *value.value;
}

[[nodiscard]] std::optional<int> UnwrapOptionalMoveOnly(bool ok)
{
    auto value = TRY(MaybePointer(ok));
    return *value;
}

[[nodiscard]] ws::Expected<int, TestError> MapExpectedValue(bool ok)
{
    return TRY_MAP(MakeValue(ok), [](int value) { return value * 2; }) + 1;
}

[[nodiscard]] ws::Expected<int, int> MapExpectedError(bool ok)
{
    return TRY_MAP_ERR(MakeValue(ok), [](auto err) { return err == TestError::kAlpha ? 10 : 20; }) *
           2;
}

[[nodiscard]] ws::Expected<int, int> MapVoidError(bool ok)
{
    TRY_MAP_ERR(MakeVoid(ok), [](auto err) { return err == TestError::kBeta ? 30 : 40; });
    return 9;
}

[[nodiscard]] ws::Expected<int, int> ReplaceExpectedError(bool ok, int &err_calls)
{
    return TRY_ERR_AS(MakeValue(ok), ([&]() {
                          err_calls++;
                          return 95;
                      })()) +
           1;
}

[[nodiscard]] ws::Expected<int, int> ReplaceVoidError(bool ok)
{
    TRY_ERR_AS(MakeVoid(ok), 96);
    return 9;
}

[[nodiscard]] ws::Expected<int, int>
MapErrorSingleEvaluation(int &expr_calls, int &mapper_calls, bool ok)
{
    return TRY_MAP_ERR(
               ([&]() -> ws::Expected<int, TestError> {
                   expr_calls++;
                   if (!ok)
                       return ws::Unex(TestError::kAlpha);
                   return 4;
               })(),
               [&](auto err) {
                   mapper_calls++;
                   return err == TestError::kAlpha ? 50 : 60;
               }
           ) +
           1;
}

[[nodiscard]] ws::Expected<int, int> OkOrOptional(bool ok)
{
    return TRY_OK_OR(MaybeValue(ok), 70) + 1;
}

[[nodiscard]] ws::Expected<int, int> OkOrElseOptional(bool ok, int &err_calls)
{
    return TRY_OK_OR_ELSE(
               MaybeValue(ok),
               [&]() {
                   err_calls++;
                   return 80;
               }
           ) +
           1;
}

[[nodiscard]] ws::Expected<int, int> EnsureValue(bool ok, int &err_calls)
{
    ENSURE(ok, ([&]() {
               err_calls++;
               return 90;
           })());
    return 3;
}

} // namespace

UTEST(Try, PropagatesExpectedValueErrors)
{
    auto value = DoubleExpected(false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), TestError::kAlpha);
}

UTEST(Try, UnwrapsExpectedValueInExpressionPosition)
{
    auto value = DoubleExpected(true);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 42);
}

UTEST(Try, PropagatesExpectedVoidErrors)
{
    auto value = PropagateVoid(false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), TestError::kBeta);
}

UTEST(Try, SupportsExpectedVoidStatements)
{
    auto value = PropagateVoid(true);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 9);
}

UTEST(Try, PropagatesEmptyOptional)
{
    auto value = DoubleOptional(false);
    ASSERT_FALSE(value);
}

UTEST(Try, UnwrapsOptionalInExpressionPosition)
{
    auto value = DoubleOptional(true);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 14);
}

UTEST(Try, ConvertsExpectedErrorToEmptyOptional)
{
    auto value = OptionalFromExpected(false);
    ASSERT_FALSE(value);
}

UTEST(Try, UnwrapsExpectedIntoOptional)
{
    auto value = OptionalFromExpected(true);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 42);
}

UTEST(Try, ConvertsExpectedVoidErrorToEmptyOptional)
{
    auto value = OptionalFromExpectedVoid(false);
    ASSERT_FALSE(value);
}

UTEST(Try, UnwrapsExpectedVoidIntoOptional)
{
    auto value = OptionalFromExpectedVoid(true);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 9);
}

UTEST(Try, SupportsLvalueOptional)
{
    auto value = LvalueOptional(true);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 8);
}

UTEST(Try, SupportsExpectedLvalueInOptionalReturn)
{
    auto value = LvalueOptionalFromExpected(true);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 22);
}

UTEST(Try, ConvertsExpectedLvalueErrorToEmptyOptional)
{
    auto value = LvalueOptionalFromExpected(false);
    ASSERT_FALSE(value);
}

UTEST(Try, PropagatesLvalueExpectedErrors)
{
    auto value = LvalueExpected(false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), TestError::kAlpha);
}

UTEST(Try, SupportsMultipleExpressionUses)
{
    auto value = TwoTrys(true, true);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 42);
}

UTEST(Try, StopsAtFirstError)
{
    auto value = TwoTrys(false, true);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), TestError::kAlpha);
}

UTEST(Try, EvaluatesExpressionOnceOnSuccess)
{
    int calls = 0;
    auto value = SingleEvaluation(calls, true);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 5);
    EXPECT_EQ(calls, 1);
}

UTEST(Try, EvaluatesExpressionOnceOnError)
{
    int calls = 0;
    auto value = SingleEvaluation(calls, false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), TestError::kBeta);
    EXPECT_EQ(calls, 1);
}

UTEST(Try, UnwrapsMoveOnlyExpectedValues)
{
    auto value = UnwrapMoveOnly(true);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 33);
}

UTEST(Try, UnwrapsMoveOnlyOptionalValues)
{
    auto value = UnwrapOptionalMoveOnly(true);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 13);
}

UTEST(Try, PropagatesEmptyOptionalForMoveOnlyValues)
{
    auto value = UnwrapOptionalMoveOnly(false);
    ASSERT_FALSE(value);
}

UTEST(Try, MapPreservesExpectedError)
{
    auto value = MapExpectedValue(false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), TestError::kAlpha);
}

UTEST(Try, MapTransformsExpectedSuccessValue)
{
    auto value = MapExpectedValue(true);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 43);
}

UTEST(Try, MapErrPreservesExpectedSuccessValue)
{
    auto value = MapExpectedError(true);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 42);
}

UTEST(Try, MapErrMapsExpectedError)
{
    auto value = MapExpectedError(false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), 10);
}

UTEST(Try, MapErrSupportsExpectedVoid)
{
    auto value = MapVoidError(false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), 30);
}

UTEST(Try, ErrAsPreservesExpectedSuccessValue)
{
    int err_calls = 0;
    auto value = ReplaceExpectedError(true, err_calls);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 22);
    EXPECT_EQ(err_calls, 0);
}

UTEST(Try, ErrAsReplacesExpectedError)
{
    int err_calls = 0;
    auto value = ReplaceExpectedError(false, err_calls);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), 95);
    EXPECT_EQ(err_calls, 1);
}

UTEST(Try, ErrAsSupportsExpectedVoid)
{
    auto value = ReplaceVoidError(false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), 96);
}

UTEST(Try, MapErrEvaluatesExpressionOnceOnSuccess)
{
    int expr_calls = 0;
    int mapper_calls = 0;
    auto value = MapErrorSingleEvaluation(expr_calls, mapper_calls, true);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 5);
    EXPECT_EQ(expr_calls, 1);
    EXPECT_EQ(mapper_calls, 0);
}

UTEST(Try, MapErrEvaluatesExpressionAndMapperOnceOnError)
{
    int expr_calls = 0;
    int mapper_calls = 0;
    auto value = MapErrorSingleEvaluation(expr_calls, mapper_calls, false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), 50);
    EXPECT_EQ(expr_calls, 1);
    EXPECT_EQ(mapper_calls, 1);
}

UTEST(Try, OkOrUnwrapsOptionalSuccess)
{
    auto value = OkOrOptional(true);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 8);
}

UTEST(Try, OkOrMapsOptionalError)
{
    auto value = OkOrOptional(false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), 70);
}

UTEST(Try, OkOrElseBuildsErrorOnceOnError)
{
    int err_calls = 0;
    auto value = OkOrElseOptional(false, err_calls);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), 80);
    EXPECT_EQ(err_calls, 1);
}

UTEST(Try, OkOrElseSkipsErrorFactoryOnSuccess)
{
    int err_calls = 0;
    auto value = OkOrElseOptional(true, err_calls);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 8);
    EXPECT_EQ(err_calls, 0);
}

UTEST(Try, EnsureReturnsSuccessWhenConditionMatches)
{
    int err_calls = 0;
    auto value = EnsureValue(true, err_calls);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 3);
    EXPECT_EQ(err_calls, 0);
}

UTEST(Try, EnsureReturnsMappedErrorWhenConditionFails)
{
    int err_calls = 0;
    auto value = EnsureValue(false, err_calls);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), 90);
    EXPECT_EQ(err_calls, 1);
}
