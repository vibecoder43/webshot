#pragma once

#include "expected.hpp"

#include <boost/safe_numerics/checked_default.hpp>
#include <boost/safe_numerics/checked_result_operations.hpp>
#include <boost/safe_numerics/exception_policies.hpp>
#include <boost/safe_numerics/safe_base_operations.hpp>
#include <boost/safe_numerics/safe_common.hpp>
#include <boost/safe_numerics/safe_integer.hpp>

#include <userver/utils/assert.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ws {
namespace us = userver;
} // namespace ws

namespace integers {

struct SafeIntegerAbort {
    [[noreturn]] void
    operator()(const boost::safe_numerics::safe_numerics_error &e, const char *msg) const noexcept
    {
        const char *msg_safe = msg ? msg : "(no details)";
        const char *error_safe = boost::safe_numerics::literal_string(e);

        std::array<char, 512> buf{};
        const int written = std::snprintf(
            buf.data(), buf.size(), "safe integer failure: %s: %s",
            error_safe ? error_safe : "(unknown)", msg_safe
        );

        if (written <= 0) {
            ws::us::utils::AbortWithStacktrace("safe integer operation failed");
        }

        const size_t len = static_cast<size_t>(written) < buf.size() ? static_cast<size_t>(written)
                                                                     : (buf.size() - 1);
        ws::us::utils::AbortWithStacktrace(std::string_view{buf.data(), len});
    }
};

struct TrapUninitialized {
    template <typename... Args> constexpr void operator()(Args &&...) const
    {
        static_assert(sizeof...(Args) == 0, "safe integers must be explicitly initialized");
    }
};

using AbortPolicy = boost::safe_numerics::exception_policy<
    SafeIntegerAbort, SafeIntegerAbort, SafeIntegerAbort, TrapUninitialized>;

template <typename T>
using SafeInteger = boost::safe_numerics::safe<T, boost::safe_numerics::native, AbortPolicy>;

using u16 = SafeInteger<uint16_t>;
using u32 = SafeInteger<uint32_t>;
using i32 = SafeInteger<int32_t>;
using u64 = SafeInteger<uint64_t>;
using i64 = SafeInteger<int64_t>;
using usize = SafeInteger<size_t>;

static_assert(std::numeric_limits<u16>::is_specialized);
static_assert(std::numeric_limits<u32>::is_specialized);
static_assert(std::numeric_limits<i32>::is_specialized);
static_assert(std::numeric_limits<u64>::is_specialized);
static_assert(std::numeric_limits<i64>::is_specialized);
static_assert(std::numeric_limits<usize>::is_specialized);

static_assert(std::is_same_v<decltype(std::numeric_limits<u16>::max()), u16>);
static_assert(std::is_same_v<decltype(std::numeric_limits<u32>::max()), u32>);
static_assert(std::is_same_v<decltype(std::numeric_limits<i32>::max()), i32>);
static_assert(std::is_same_v<decltype(std::numeric_limits<u64>::max()), u64>);
static_assert(std::is_same_v<decltype(std::numeric_limits<i64>::max()), i64>);
static_assert(std::is_same_v<decltype(std::numeric_limits<usize>::max()), usize>);

template <typename T>
concept NumericCastType = std::integral<std::remove_cvref_t<T>> ||
                          std::is_enum_v<std::remove_cvref_t<T>>;

namespace detail {

enum class NumericCastError {
    kNegativeToUnsigned,
    kNarrowingOverflow,
    kEnumUnderlyingOverflow,
};

[[noreturn]] inline void AbortNumericCastFailure(NumericCastError error) noexcept
{
    using enum NumericCastError;

    switch (error) {
    case kNegativeToUnsigned:
        ws::us::utils::AbortWithStacktrace("safe integer failure: negative to unsigned");
    case kNarrowingOverflow:
        ws::us::utils::AbortWithStacktrace("safe integer failure: narrowing overflow");
    case kEnumUnderlyingOverflow:
        ws::us::utils::AbortWithStacktrace("safe integer failure: enum underlying overflow");
    default:
        ws::us::utils::AbortWithStacktrace("safe integer failure");
    }
}

template <typename To, typename From>
    requires NumericCastType<To> && NumericCastType<From>
[[nodiscard]] constexpr ws::Expected<std::remove_cvref_t<To>, NumericCastError>
CheckedNumericCast(From value) noexcept
{
    using ToValue = std::remove_cvref_t<To>;
    using FromValue = std::remove_cvref_t<From>;

    if constexpr (std::is_enum_v<ToValue>) {
        using ToUnderlying = std::underlying_type_t<ToValue>;
        auto underlying_value = CheckedNumericCast<ToUnderlying>(value);
        if (!underlying_value)
            return ws::Unex(NumericCastError::kEnumUnderlyingOverflow);
        return static_cast<ToValue>(*underlying_value);
    } else if constexpr (std::is_enum_v<FromValue>) {
        return CheckedNumericCast<ToValue>(std::to_underlying(value));
    } else {
        if constexpr (std::is_unsigned_v<ToValue> && std::is_signed_v<FromValue>) {
            if (value < 0)
                return ws::Unex(NumericCastError::kNegativeToUnsigned);
        }
        if (!std::in_range<ToValue>(value))
            return ws::Unex(NumericCastError::kNarrowingOverflow);
        return static_cast<ToValue>(value);
    }
}

template <typename To, typename T, typename PromotionPolicy, typename ExceptionPolicy>
[[nodiscard]] constexpr ws::Expected<std::remove_cvref_t<To>, NumericCastError> CheckedNumericCast(
    const boost::safe_numerics::safe<T, PromotionPolicy, ExceptionPolicy> &value
) noexcept
    requires NumericCastType<To>
{
    using ToValue = std::remove_cvref_t<To>;

    if constexpr (std::same_as<ToValue, T>) {
        return static_cast<T>(value);
    } else {
        return CheckedNumericCast<To>(static_cast<T>(value));
    }
}

} // namespace detail

template <typename To, typename From>
    requires NumericCastType<To> && NumericCastType<From>
[[nodiscard]] constexpr std::remove_cvref_t<To> NumericCast(From value) noexcept
{
    using ToValue = std::remove_cvref_t<To>;
    const auto converted = detail::CheckedNumericCast<ToValue>(value);
    if (!converted)
        detail::AbortNumericCastFailure(converted.Error());
    return *converted;
}

template <typename T, typename PromotionPolicy, typename ExceptionPolicy>
[[nodiscard]] constexpr T
NumericCast(const boost::safe_numerics::safe<T, PromotionPolicy, ExceptionPolicy> &value) noexcept
{
    return static_cast<T>(value);
}

template <typename To, typename T, typename PromotionPolicy, typename ExceptionPolicy>
[[nodiscard]] constexpr std::remove_cvref_t<To>
NumericCast(const boost::safe_numerics::safe<T, PromotionPolicy, ExceptionPolicy> &value) noexcept
    requires NumericCastType<To> && (!std::same_as<std::remove_cvref_t<To>, T>)
{
    using ToValue = std::remove_cvref_t<To>;
    const auto converted = detail::CheckedNumericCast<ToValue>(value);
    if (!converted)
        detail::AbortNumericCastFailure(converted.Error());
    return *converted;
}

template <typename T, typename PromotionPolicy, typename ExceptionPolicy>
[[nodiscard]] constexpr T
Raw(const boost::safe_numerics::safe<T, PromotionPolicy, ExceptionPolicy> &value) noexcept
{
    return NumericCast(value);
}

struct SSizeFn {
    template <typename C> [[nodiscard]] constexpr i64 operator()(const C &c) const noexcept
    {
        const auto size_value = usize(c.size());
        return i64(size_value);
    }
};

struct USizeFn {
    template <typename C>
        requires requires(const C &c) { c.size(); }
    [[nodiscard]] constexpr usize operator()(const C &c) const noexcept
    {
        return usize(c.size());
    }
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline constexpr SSizeFn ssize{};
// NOLINTNEXTLINE(readability-identifier-naming)
inline constexpr USizeFn usz{};

} // namespace integers

namespace integers::literals {

[[nodiscard]] constexpr u16 operator""_u16(unsigned long long value) noexcept { return u16(value); }

[[nodiscard]] constexpr u32 operator""_u32(unsigned long long value) noexcept { return u32(value); }

[[nodiscard]] constexpr i32 operator""_i32(unsigned long long value) noexcept { return i32(value); }

[[nodiscard]] constexpr u64 operator""_u64(unsigned long long value) noexcept { return u64(value); }

[[nodiscard]] constexpr i64 operator""_i64(unsigned long long value) noexcept { return i64(value); }

[[nodiscard]] constexpr usize operator""_uz(unsigned long long value) noexcept
{
    return usize(value);
}

} // namespace integers::literals

using integers::i32;
using integers::i64;
using integers::NumericCast;
using integers::Raw;
using integers::SafeInteger;
using integers::ssize;
using integers::u16;
using integers::u32;
using integers::u64;
using integers::usize;
using integers::usz;
using namespace integers::literals;

template <> struct std::formatter<u16, char> : std::formatter<uint16_t, char> {
    auto format(const u16 &value, std::format_context &ctx) const
    {
        return std::formatter<uint16_t, char>::format(integers::Raw(value), ctx);
    }
};

template <> struct std::formatter<u32, char> : std::formatter<uint32_t, char> {
    auto format(const u32 &value, std::format_context &ctx) const
    {
        return std::formatter<uint32_t, char>::format(integers::Raw(value), ctx);
    }
};

template <> struct std::formatter<i32, char> : std::formatter<int32_t, char> {
    auto format(const i32 &value, std::format_context &ctx) const
    {
        return std::formatter<int32_t, char>::format(integers::Raw(value), ctx);
    }
};

template <> struct std::formatter<u64, char> : std::formatter<uint64_t, char> {
    auto format(const u64 &value, std::format_context &ctx) const
    {
        return std::formatter<uint64_t, char>::format(integers::Raw(value), ctx);
    }
};

template <> struct std::formatter<i64, char> : std::formatter<int64_t, char> {
    auto format(const i64 &value, std::format_context &ctx) const
    {
        return std::formatter<int64_t, char>::format(integers::Raw(value), ctx);
    }
};

template <> struct std::hash<u16> {
    size_t operator()(const u16 &value) const noexcept
    {
        return std::hash<uint16_t>{}(integers::Raw(value));
    }
};

template <> struct std::hash<u32> {
    size_t operator()(const u32 &value) const noexcept
    {
        return std::hash<uint32_t>{}(integers::Raw(value));
    }
};

template <> struct std::hash<i32> {
    size_t operator()(const i32 &value) const noexcept
    {
        return std::hash<int32_t>{}(integers::Raw(value));
    }
};

template <> struct std::hash<u64> {
    size_t operator()(const u64 &value) const noexcept
    {
        return std::hash<uint64_t>{}(integers::Raw(value));
    }
};

template <> struct std::hash<i64> {
    size_t operator()(const i64 &value) const noexcept
    {
        return std::hash<int64_t>{}(integers::Raw(value));
    }
};
