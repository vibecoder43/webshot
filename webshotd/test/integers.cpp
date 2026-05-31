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
    EXPECT_EQ(NumericCast<int64_t>(123), int64_t{123});
    EXPECT_EQ(NumericCast<size_t>(int64_t{3}), size_t{3});
    EXPECT_EQ(NumericCast<int>(size_t{7}), 7);
}

UTEST(Integers, NumericCastSupportsEnumConversions)
{
    EXPECT_EQ(NumericCast<unsigned int>(SmallEnum::kTwo), 2U);
    EXPECT_EQ(NumericCast<SmallEnum>(std::uint16_t{2}), SmallEnum::kTwo);
}

UTEST(Integers, NumericCastSupportsSafeIntegerSources)
{
    const i64 value{123};
    const u16 enum_value{2};

    EXPECT_EQ(NumericCast<int64_t>(value), int64_t{123});
    EXPECT_EQ(NumericCast<SmallEnum>(enum_value), SmallEnum::kTwo);
}

UTEST(Integers, CheckedNumericCastReportsNegativeToUnsigned)
{
    auto result = integers::detail::CheckedNumericCast<size_t>(-1);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.Error(), integers::detail::NumericCastError::kNegativeToUnsigned);
}

UTEST(Integers, CheckedNumericCastReportsNarrowingOverflow)
{
    auto result = integers::detail::CheckedNumericCast<int>(std::numeric_limits<int64_t>::max());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.Error(), integers::detail::NumericCastError::kNarrowingOverflow);
}

UTEST(Integers, CheckedNumericCastReportsEnumUnderlyingOverflow)
{
    auto result = integers::detail::CheckedNumericCast<SmallEnum>(std::uint16_t{256});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.Error(), integers::detail::NumericCastError::kEnumUnderlyingOverflow);
}
