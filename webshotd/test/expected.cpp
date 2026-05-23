#include <expected>

#include <userver/utest/utest.hpp>

#include "expected.hpp"

namespace {

enum class TestError {
    kAlpha,
    kBeta,
};

[[nodiscard]] ws::Expected<int, TestError> MakeValue(bool ok)
{
    if (!ok)
        return ws::Unex(TestError::kAlpha);
    return 42;
}

[[nodiscard]] ws::Expected<void, TestError> MakeVoid(bool ok)
{
    if (!ok)
        return ws::Unex(TestError::kBeta);
    return {};
}

[[nodiscard]] ws::Expected<void, TestError> PropagateValueError(bool ok)
{
    auto value = MakeValue(ok);
    if (!value)
        return ws::Unex(value.Error());
    return {};
}

[[nodiscard]] ws::Expected<int, TestError> PropagateVoidError(bool ok)
{
    auto result = MakeVoid(ok);
    if (!result)
        return ws::Unex(result.Error());
    return 7;
}

} // namespace

UTEST(Expected, UnexBuildsValueExpectedError)
{
    auto value = MakeValue(false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), TestError::kAlpha);
}

UTEST(Expected, UnexBuildsVoidExpectedError)
{
    auto value = MakeVoid(false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), TestError::kBeta);
}

UTEST(Expected, ExplicitErrorPropagationFromValueExpected)
{
    auto value = PropagateValueError(false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), TestError::kAlpha);
}

UTEST(Expected, ExplicitErrorPropagationFromVoidExpected)
{
    auto value = PropagateVoidError(false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), TestError::kBeta);
}

UTEST(Expected, TransformAndThenAndTransformErrorStillWork)
{
    auto transformed = MakeValue(true).Transform(
                                          [](int value) { return value + 1; }
    ).AndThen([](int value) -> ws::Expected<int, TestError> { return value * 2; });
    ASSERT_TRUE(transformed);
    EXPECT_EQ(*transformed, 86);

    auto mapped_error = MakeValue(false).TransformError([](TestError error) {
        return error == TestError::kAlpha;
    });
    ASSERT_FALSE(mapped_error);
    EXPECT_TRUE(mapped_error.Error());
}

UTEST(Expected, AcceptsStdUnexpectedForCompatibility)
{
    const ws::Expected<int, TestError> value{std::unexpected(TestError::kBeta)};
    ASSERT_FALSE(value);
    EXPECT_EQ(value.Error(), TestError::kBeta);
}
