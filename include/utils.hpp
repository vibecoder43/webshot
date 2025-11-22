#pragma once

/**
 * @file
 * @brief Small utility helpers
 */

#include <type_traits>

/**
 * @brief Signed size of a container.
 */
namespace v1::utils {
template <class C>
constexpr auto ssize(const C &c)
    -> std::common_type_t<std::ptrdiff_t, std::make_signed_t<decltype(c.size())>>
{
    using R = std::common_type_t<std::ptrdiff_t, std::make_signed_t<decltype(c.size())>>;
    return static_cast<R>(c.size());
}
}; // namespace v1::utils
