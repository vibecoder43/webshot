#pragma once

#include <boost/safe_numerics/exception_policies.hpp>
#include <boost/safe_numerics/safe_integer.hpp>

#include <userver/utils/assert.hpp>
#include <userver/utils/numeric_cast.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <limits>
#include <string_view>
#include <type_traits>

namespace integers_detail {

struct Abort {
    [[noreturn]] void
    operator()(const boost::safe_numerics::safe_numerics_error &e, const char *msg) const noexcept
    {
        const char *msgSafe = msg ? msg : "(no details)";
        const char *errorSafe = boost::safe_numerics::literal_string(e);

        std::array<char, 512> buf{};
        const int written = std::snprintf(
            buf.data(), buf.size(), "safe integer failure: %s: %s",
            errorSafe ? errorSafe : "(unknown)", msgSafe
        );

        if (written <= 0) {
            userver::utils::AbortWithStacktrace("safe integer operation failed");
        }

        const size_t len = static_cast<size_t>(written) < buf.size() ? static_cast<size_t>(written)
                                                                     : (buf.size() - 1);
        userver::utils::AbortWithStacktrace(std::string_view{buf.data(), len});
    }
};

struct TrapUninitialized {
    template <typename... Args> constexpr void operator()(Args &&...) const
    {
        static_assert(sizeof...(Args) == 0, "safe integers must be explicitly initialized");
    }
};

using AbortPolicy = boost::safe_numerics::exception_policy<Abort, Abort, Abort, TrapUninitialized>;

} // namespace integers_detail

using u32 = boost::safe_numerics::safe<
    uint32_t, boost::safe_numerics::native, integers_detail::AbortPolicy>;
using i32 =
    boost::safe_numerics::safe<int32_t, boost::safe_numerics::native, integers_detail::AbortPolicy>;
using u64 = boost::safe_numerics::safe<
    uint64_t, boost::safe_numerics::native, integers_detail::AbortPolicy>;
using i64 =
    boost::safe_numerics::safe<int64_t, boost::safe_numerics::native, integers_detail::AbortPolicy>;
using usize =
    boost::safe_numerics::safe<size_t, boost::safe_numerics::native, integers_detail::AbortPolicy>;

static_assert(std::numeric_limits<u32>::is_specialized);
static_assert(std::numeric_limits<i32>::is_specialized);
static_assert(std::numeric_limits<u64>::is_specialized);
static_assert(std::numeric_limits<i64>::is_specialized);
static_assert(std::numeric_limits<usize>::is_specialized);

static_assert(std::is_same_v<decltype(std::numeric_limits<u32>::max()), u32>);
static_assert(std::is_same_v<decltype(std::numeric_limits<i32>::max()), i32>);
static_assert(std::is_same_v<decltype(std::numeric_limits<u64>::max()), u64>);
static_assert(std::is_same_v<decltype(std::numeric_limits<i64>::max()), i64>);
static_assert(std::is_same_v<decltype(std::numeric_limits<usize>::max()), usize>);

namespace integers {

template <typename To, typename From> [[nodiscard]] constexpr To numericCast(From value)
{
    if constexpr (std::is_enum_v<To>) {
        using ToUnderlying = std::underlying_type_t<To>;
        return static_cast<To>(numericCast<ToUnderlying>(value));
    } else if constexpr (std::is_enum_v<From>) {
        using FromUnderlying = std::underlying_type_t<From>;
        return numericCast<To>(static_cast<FromUnderlying>(value));
    } else {
        return userver::utils::numeric_cast<To>(value);
    }
}

template <typename T, typename PromotionPolicy, typename ExceptionPolicy>
[[nodiscard]] constexpr T
numericCast(const boost::safe_numerics::safe<T, PromotionPolicy, ExceptionPolicy> &value) noexcept
{
    return static_cast<T>(value);
}

template <typename To, typename T, typename PromotionPolicy, typename ExceptionPolicy>
[[nodiscard]] constexpr std::enable_if_t<!std::is_same_v<To, T>, To>
numericCast(const boost::safe_numerics::safe<T, PromotionPolicy, ExceptionPolicy> &value)
{
    if constexpr (std::is_enum_v<To>) {
        using ToUnderlying = std::underlying_type_t<To>;
        return static_cast<To>(numericCast<ToUnderlying>(numericCast(value)));
    } else {
        return userver::utils::numeric_cast<To>(numericCast(value));
    }
}

template <typename T, typename PromotionPolicy, typename ExceptionPolicy>
[[nodiscard]] constexpr T
raw(const boost::safe_numerics::safe<T, PromotionPolicy, ExceptionPolicy> &value) noexcept
{
    return numericCast(value);
}

struct SSizeFn {
    template <typename C> [[nodiscard]] constexpr i64 operator()(const C &c) const noexcept
    {
        const auto sizeValue = usize(c.size());
        return i64(sizeValue);
    }
};

inline constexpr SSizeFn ssize{};

} // namespace integers

namespace integers::literals {

[[nodiscard]] constexpr u32 operator""_u32(unsigned long long value) noexcept { return u32(value); }

[[nodiscard]] constexpr i32 operator""_i32(unsigned long long value) noexcept { return i32(value); }

[[nodiscard]] constexpr u64 operator""_u64(unsigned long long value) noexcept { return u64(value); }

[[nodiscard]] constexpr i64 operator""_i64(unsigned long long value) noexcept { return i64(value); }

} // namespace integers::literals

using integers::numericCast;
using integers::raw;
using integers::ssize;
using namespace integers::literals;

namespace std {

template <> struct formatter<u32, char> : formatter<uint32_t, char> {
    auto format(const u32 &value, format_context &ctx) const
    {
        return formatter<uint32_t, char>::format(integers::raw(value), ctx);
    }
};

template <> struct formatter<i32, char> : formatter<int32_t, char> {
    auto format(const i32 &value, format_context &ctx) const
    {
        return formatter<int32_t, char>::format(integers::raw(value), ctx);
    }
};

template <> struct formatter<u64, char> : formatter<uint64_t, char> {
    auto format(const u64 &value, format_context &ctx) const
    {
        return formatter<uint64_t, char>::format(integers::raw(value), ctx);
    }
};

template <> struct formatter<i64, char> : formatter<int64_t, char> {
    auto format(const i64 &value, format_context &ctx) const
    {
        return formatter<int64_t, char>::format(integers::raw(value), ctx);
    }
};

} // namespace std
