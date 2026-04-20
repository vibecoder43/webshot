#include "integers.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

#include <userver/utest/utest.hpp>

namespace {

enum class SmallEnum : std::uint8_t {
    kZero = 0,
    kTwo = 2,
};

} // namespace

UTEST(Integers, NumericCastSupportsIntegralConversions)
{
    EXPECT_EQ(numericCast<int64_t>(123), int64_t{123});
    EXPECT_EQ(numericCast<size_t>(int64_t{3}), size_t{3});
    EXPECT_EQ(numericCast<int>(size_t{7}), 7);
}

UTEST(Integers, NumericCastSupportsEnumConversions)
{
    EXPECT_EQ(numericCast<unsigned int>(SmallEnum::kTwo), 2U);
    EXPECT_EQ(numericCast<SmallEnum>(std::uint16_t{2}), SmallEnum::kTwo);
}

UTEST(Integers, NumericCastSupportsSafeIntegerSources)
{
    const i64 value{123};
    const u16 enumValue{2};

    EXPECT_EQ(numericCast<int64_t>(value), int64_t{123});
    EXPECT_EQ(numericCast<SmallEnum>(enumValue), SmallEnum::kTwo);
}

UTEST(Integers, CheckedNumericCastReportsNegativeToUnsigned)
{
    const auto result = integers::detail::checkedNumericCast<size_t>(-1);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), integers::detail::NumericCastError::kNegativeToUnsigned);
}

UTEST(Integers, CheckedNumericCastReportsNarrowingOverflow)
{
    const auto result = integers::detail::checkedNumericCast<int>(
        std::numeric_limits<int64_t>::max()
    );
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), integers::detail::NumericCastError::kNarrowingOverflow);
}

UTEST(Integers, CheckedNumericCastReportsEnumUnderlyingOverflow)
{
    const auto result = integers::detail::checkedNumericCast<SmallEnum>(std::uint16_t{256});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), integers::detail::NumericCastError::kEnumUnderlyingOverflow);
}
