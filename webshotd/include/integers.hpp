#pragma once

#include <boost/safe_numerics/exception_policies.hpp>
#include <boost/safe_numerics/safe_integer.hpp>

#include <fmt/format.h>

#include <userver/utils/numeric_cast.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <type_traits>

// fmt v12 stores formatting arguments in an internal `fmt::detail::value<Context>`.
// boost::safe_numerics::safe_base has a templated conversion operator `operator R()`
// enabled for any `R` that is not marked as "safe" (boost::safe_numerics::is_safe<R>).
// That makes safe integers implicitly convertible to fmt internals, which clashes with
// fmt's own argument handling and results in an ambiguous conversion error.
//
// Mark fmt's internal value type as "safe" to SFINAE-out that conversion path.
namespace boost::safe_numerics {
template <typename Context> struct is_safe<fmt::detail::value<Context>> : std::true_type {};
} // namespace boost::safe_numerics

namespace integers_detail {

struct Abort {
    [[noreturn]] void
    operator()(const boost::safe_numerics::safe_numerics_error &, const char *) const noexcept
    {
        std::abort();
    }
};

struct TrapUninitialized {
    template <typename... Args> constexpr void operator()(Args &&...) const
    {
        static_assert(
            sizeof...(Args) == 0,
            "safe integers must be explicitly initialized (default initialization is forbidden)"
        );
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

template <class C> [[nodiscard]] constexpr i64 safeSize(const C &c) noexcept
{
    const auto sizeValue = usize(c.size());
    return i64(sizeValue);
}

} // namespace integers

template <typename T, typename PromotionPolicy, typename ExceptionPolicy>
struct fmt::formatter<boost::safe_numerics::safe<T, PromotionPolicy, ExceptionPolicy>>
    : fmt::formatter<T> {
    fmt::format_context::iterator format(
        const boost::safe_numerics::safe<T, PromotionPolicy, ExceptionPolicy> &value,
        fmt::format_context &ctx
    ) const
    {
        return fmt::formatter<T>::format(integers::raw(value), ctx);
    }
};

namespace integers::literals {

[[nodiscard]] constexpr u32 operator""_u32(unsigned long long value) noexcept { return u32(value); }

[[nodiscard]] constexpr i32 operator""_i32(unsigned long long value) noexcept { return i32(value); }

[[nodiscard]] constexpr u64 operator""_u64(unsigned long long value) noexcept { return u64(value); }

[[nodiscard]] constexpr i64 operator""_i64(unsigned long long value) noexcept { return i64(value); }

} // namespace integers::literals

using integers::numericCast;
using integers::raw;
using integers::safeSize;
using namespace integers::literals;
