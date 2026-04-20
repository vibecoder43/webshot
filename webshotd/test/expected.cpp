#include <expected>

#include <userver/utest/utest.hpp>

#include "expected.hpp"

namespace {

enum class TestError {
    kAlpha,
    kBeta,
};

[[nodiscard]] v1::Expected<int, TestError> makeValue(bool ok)
{
    if (!ok)
        return v1::Unex(TestError::kAlpha);
    return 42;
}

[[nodiscard]] v1::Expected<void, TestError> makeVoid(bool ok)
{
    if (!ok)
        return v1::Unex(TestError::kBeta);
    return {};
}

[[nodiscard]] v1::Expected<void, TestError> propagateValueError(bool ok)
{
    const auto value = makeValue(ok);
    if (!value)
        return v1::Unex(value.error());
    return {};
}

[[nodiscard]] v1::Expected<int, TestError> propagateVoidError(bool ok)
{
    const auto result = makeVoid(ok);
    if (!result)
        return v1::Unex(result.error());
    return 7;
}

} // namespace

UTEST(Expected, UnexBuildsValueExpectedError)
{
    const auto value = makeValue(false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.error(), TestError::kAlpha);
}

UTEST(Expected, UnexBuildsVoidExpectedError)
{
    const auto value = makeVoid(false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.error(), TestError::kBeta);
}

UTEST(Expected, ExplicitErrorPropagationFromValueExpected)
{
    const auto value = propagateValueError(false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.error(), TestError::kAlpha);
}

UTEST(Expected, ExplicitErrorPropagationFromVoidExpected)
{
    const auto value = propagateVoidError(false);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.error(), TestError::kBeta);
}

UTEST(Expected, TransformAndThenAndTransformErrorStillWork)
{
    const auto transformed = makeValue(true).transform(
                                                [](int value) { return value + 1; }
    ).andThen([](int value) -> v1::Expected<int, TestError> { return value * 2; });
    ASSERT_TRUE(transformed);
    EXPECT_EQ(*transformed, 86);

    const auto mappedError = makeValue(false).transformError([](TestError error) {
        return error == TestError::kAlpha;
    });
    ASSERT_FALSE(mappedError);
    EXPECT_TRUE(mappedError.error());
}

UTEST(Expected, AcceptsStdUnexpectedForCompatibility)
{
    const v1::Expected<int, TestError> value{std::unexpected(TestError::kBeta)};
    ASSERT_FALSE(value);
    EXPECT_EQ(value.error(), TestError::kBeta);
}
