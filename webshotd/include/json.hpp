#pragma once

#include "expected.hpp"
#include "grab_value.hpp"
#include "text.hpp"
#include "try.hpp"
#include "userver_namespaces.hpp"

#include <concepts>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <userver/formats/json.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/utils/traceful_exception.hpp>

namespace v1::exu {

namespace detail {

template <typename F> using InvokeResult = std::invoke_result_t<F>;

template <typename F> using ExpectedValue = std::remove_cvref_t<InvokeResult<F>>;

template <typename T>
concept JsonTextInput = std::same_as<std::remove_cvref_t<T>, String> ||
                        std::convertible_to<const T &, std::string_view>;

template <typename Exception, typename E, typename F, typename G>
concept SupportedErrorMapper =
    std::invocable<F> && std::invocable<G, const Exception &> &&
    std::constructible_from<E, std::invoke_result_t<G, const Exception &>>;

template <typename Exception, typename E, typename F, typename G>
[[nodiscard]] auto catchExceptionImpl(F &&f, G &&mapError) -> Expected<ExpectedValue<F>, E>
{
    try {
        if constexpr (std::is_void_v<InvokeResult<F>>) {
            std::invoke(std::forward<F>(f));
            return {};
        } else {
            return std::invoke(std::forward<F>(f));
        }
    } catch (const Exception &e) {
        return Unex(E(std::invoke(std::forward<G>(mapError), e)));
    }
}

} // namespace detail

template <typename Exception, typename E, typename F, typename G>
    requires detail::SupportedErrorMapper<Exception, E, F, G>
[[nodiscard]] auto catchException(F &&f, G &&mapError) -> Expected<detail::ExpectedValue<F>, E>
{
    return detail::catchExceptionImpl<Exception, E>(std::forward<F>(f), std::forward<G>(mapError));
}

template <typename Exception, typename E, typename F>
    requires std::invocable<F> && std::copy_constructible<E>
[[nodiscard]] auto catchException(F &&f, E error) -> Expected<detail::ExpectedValue<F>, E>
{
    return catchException<Exception, E>(
        std::forward<F>(f), [error = std::move(error)](const Exception &) { return error; }
    );
}

template <typename E, typename F, typename G>
    requires detail::SupportedErrorMapper<us::utils::TracefulException, E, F, G>
[[nodiscard]] auto catchUserver(F &&f, G &&mapError) -> Expected<detail::ExpectedValue<F>, E>
{
    return detail::catchExceptionImpl<us::utils::TracefulException, E>(
        std::forward<F>(f), std::forward<G>(mapError)
    );
}

template <typename E, typename F>
    requires std::invocable<F> && std::copy_constructible<E>
[[nodiscard]] auto catchUserver(F &&f, E error) -> Expected<detail::ExpectedValue<F>, E>
{
    return catchException<us::utils::TracefulException, E>(std::forward<F>(f), std::move(error));
}

namespace json {

template <typename T, typename E, typename G>
    requires std::invocable<G, const ::json::Exception &> &&
             std::constructible_from<E, std::invoke_result_t<G, const ::json::Exception &>>
[[nodiscard]] Expected<T, E> parse(const String &jsonText, G &&mapError)
{
    return catchException<::json::Exception, E>(
        [&]() { return ::json::FromString(jsonText.view()).template As<T>(); },
        std::forward<G>(mapError)
    );
}

template <typename T, typename E>
    requires std::copy_constructible<E>
[[nodiscard]] Expected<T, E> parse(const String &jsonText, E error)
{
    return catchException<::json::Exception, E>(
        [&]() { return ::json::FromString(jsonText.view()).template As<T>(); }, std::move(error)
    );
}

template <typename T, typename E, typename G>
    requires std::invocable<G, const ::json::Exception &> &&
             std::constructible_from<E, std::invoke_result_t<G, const ::json::Exception &>>
[[nodiscard]] Expected<T, E> as(const ::json::Value &value, G &&mapError)
{
    return catchException<::json::Exception, E>(
        [&]() { return value.template As<T>(); }, std::forward<G>(mapError)
    );
}

template <typename T, typename E>
    requires std::copy_constructible<E>
[[nodiscard]] Expected<T, E> as(const ::json::Value &value, E error)
{
    return catchException<::json::Exception, E>(
        [&]() { return value.template As<T>(); }, std::move(error)
    );
}

template <typename T, typename E, typename G>
    requires std::invocable<G, const us::utils::TracefulException &> &&
             std::constructible_from<
                 E, std::invoke_result_t<G, const us::utils::TracefulException &>>
[[nodiscard]] Expected<::json::Value, E> valueOf(const T &value, G &&mapError)
{
    return catchUserver<E>(
        [&]() { return ::json::ValueBuilder(value).ExtractValue(); }, std::forward<G>(mapError)
    );
}

template <typename T, typename E>
    requires std::copy_constructible<E>
[[nodiscard]] Expected<::json::Value, E> valueOf(const T &value, E error)
{
    return catchUserver<E>(
        [&]() { return ::json::ValueBuilder(value).ExtractValue(); }, std::move(error)
    );
}

template <typename E, typename G>
    requires std::invocable<G, const us::utils::TracefulException &> &&
             std::constructible_from<
                 E, std::invoke_result_t<G, const us::utils::TracefulException &>>
[[nodiscard]] Expected<std::string, E> stringifyBytes(::json::Value value, G &&mapError)
{
    return catchUserver<E>(
        [&]() { return ::json::ToString(std::move(value)); }, std::forward<G>(mapError)
    );
}

template <typename E>
    requires std::copy_constructible<E>
[[nodiscard]] Expected<std::string, E> stringifyBytes(::json::Value value, E error)
{
    return catchUserver<E>([&]() { return ::json::ToString(std::move(value)); }, std::move(error));
}

template <typename T, typename E, typename G>
    requires std::copy_constructible<std::remove_cvref_t<G>> && (!detail::JsonTextInput<T>)
[[nodiscard]] Expected<std::string, E> stringifyBytes(const T &value, G &&mapError)
{
    auto mapper = std::forward<G>(mapError);
    return stringifyBytes<E>(TRY(valueOf<T, E>(value, mapper)), mapper);
}

template <typename T, typename E>
    requires std::copy_constructible<E> && (!detail::JsonTextInput<T>)
[[nodiscard]] Expected<std::string, E> stringifyBytes(const T &value, E error)
{
    return stringifyBytes<E>(TRY(valueOf<T, E>(value, error)), std::move(error));
}

template <typename E, typename G>
    requires std::invocable<G, const us::utils::TracefulException &> &&
             std::constructible_from<
                 E, std::invoke_result_t<G, const us::utils::TracefulException &>>
[[nodiscard]] Expected<String, E> stringify(::json::Value value, G &&mapError)
{
    auto jsonBytes = TRY(stringifyBytes<E>(std::move(value), std::forward<G>(mapError)));
    return String::fromBytes(jsonBytes).expect();
}

template <typename E>
    requires std::copy_constructible<E>
[[nodiscard]] Expected<String, E> stringify(::json::Value value, E error)
{
    auto jsonBytes = TRY(stringifyBytes<E>(std::move(value), std::move(error)));
    return String::fromBytes(jsonBytes).expect();
}

template <typename T, typename E, typename G>
    requires std::copy_constructible<std::remove_cvref_t<G>> && (!detail::JsonTextInput<T>)
[[nodiscard]] Expected<String, E> stringify(const T &value, G &&mapError)
{
    auto mapper = std::forward<G>(mapError);
    return stringify<E>(TRY(valueOf<T, E>(value, mapper)), mapper);
}

template <typename T, typename E>
    requires std::copy_constructible<E> && (!detail::JsonTextInput<T>)
[[nodiscard]] Expected<String, E> stringify(const T &value, E error)
{
    return stringify<E>(TRY(valueOf<T, E>(value, error)), std::move(error));
}

} // namespace json

} // namespace v1::exu
