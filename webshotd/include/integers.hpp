#pragma once

#include <boost/safe_numerics/exception_policies.hpp>
#include <boost/safe_numerics/safe_integer.hpp>

#include <fmt/format.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace integers_detail {

struct Abort {
    [[noreturn]] void
    operator()(const boost::safe_numerics::safe_numerics_error &, const char *) const noexcept
    {
        std::abort();
    }
};

using AbortPolicy = boost::safe_numerics::exception_policy<Abort, Abort, Abort, Abort>;

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

template <typename T, typename PromotionPolicy, typename ExceptionPolicy>
[[nodiscard]] constexpr T
toNative(const boost::safe_numerics::safe<T, PromotionPolicy, ExceptionPolicy> &value) noexcept
{
    return static_cast<T>(value);
}

template <class C> [[nodiscard]] constexpr i64 safeSize(const C &c) noexcept
{
    const auto sizeValue = usize(c.size());
    return i64(sizeValue);
}

[[nodiscard]] constexpr size_t toSize(i64 value) noexcept
{
    return static_cast<size_t>(toNative(value));
}

[[nodiscard]] constexpr std::chrono::seconds toSeconds(i64 value) noexcept
{
    return std::chrono::seconds(toNative(value));
}

[[nodiscard]] constexpr std::chrono::milliseconds toMilliseconds(i64 value) noexcept
{
    return std::chrono::milliseconds(toNative(value));
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
        return fmt::formatter<T>::format(static_cast<T>(value), ctx);
    }
};

namespace integers::literals {

[[nodiscard]] constexpr u32 operator""_u32(unsigned long long value) noexcept { return u32(value); }

[[nodiscard]] constexpr i32 operator""_i32(unsigned long long value) noexcept { return i32(value); }

[[nodiscard]] constexpr u64 operator""_u64(unsigned long long value) noexcept { return u64(value); }

[[nodiscard]] constexpr i64 operator""_i64(unsigned long long value) noexcept { return i64(value); }

} // namespace integers::literals

using integers::safeSize;
using integers::toMilliseconds;
using integers::toNative;
using integers::toSeconds;
using integers::toSize;
using namespace integers::literals;
