#pragma once

#include "expected.hpp"

#include <concepts>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <userver/crypto/base64.hpp>
#include <userver/crypto/exception.hpp>

namespace ws::crypto {

namespace us = userver;
namespace detail {

template <typename E, typename G>
concept CryptoErrorMapper =
    std::invocable<G, const us::crypto::CryptoException &> &&
    std::constructible_from<E, std::invoke_result_t<G, const us::crypto::CryptoException &>>;

template <typename E, typename G, typename F>
    requires CryptoErrorMapper<E, G> && std::invocable<F>
[[nodiscard]] Expected<std::string, E> CatchCrypto(F &&f, G &&map_error)
{
    try {
        return std::invoke(std::forward<F>(f));
    } catch (const us::crypto::CryptoException &e) {
        return Unex(E(std::invoke(std::forward<G>(map_error), e)));
    }
}

} // namespace detail

template <typename E, typename G>
    requires detail::CryptoErrorMapper<E, G>
[[nodiscard]] Expected<std::string, E> Base64Decode(std::string_view encoded, G &&map_error)
{
    return detail::CatchCrypto<E>(
        [&]() { return us::crypto::base64::Base64Decode(encoded); }, std::forward<G>(map_error)
    );
}

template <typename E>
    requires std::copy_constructible<E>
[[nodiscard]] Expected<std::string, E> Base64Decode(std::string_view encoded, E error)
{
    return Base64Decode<E>(
        encoded, [error = std::move(error)](const us::crypto::CryptoException &) { return error; }
    );
}

template <typename E, typename G>
    requires detail::CryptoErrorMapper<E, G>
[[nodiscard]] Expected<std::string, E> Base64UrlDecode(std::string_view encoded, G &&map_error)
{
    return detail::CatchCrypto<E>(
        [&]() { return us::crypto::base64::Base64UrlDecode(encoded); }, std::forward<G>(map_error)
    );
}

template <typename E>
    requires std::copy_constructible<E>
[[nodiscard]] Expected<std::string, E> Base64UrlDecode(std::string_view encoded, E error)
{
    return Base64UrlDecode<E>(
        encoded, [error = std::move(error)](const us::crypto::CryptoException &) { return error; }
    );
}

} // namespace ws::crypto
