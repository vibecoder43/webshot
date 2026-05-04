#pragma once

#include "expected.hpp"
#include "grab_value.hpp"
#include "text.hpp"
#include "try.hpp"

#include <concepts>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <userver/formats/json.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/formats/json/value_builder.hpp>

namespace ws::json {
namespace us = userver;
namespace ujson = us::formats::json;

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
[[nodiscard]] auto CatchExceptionImpl(F &&f, G &&map_error) -> Expected<ExpectedValue<F>, E>
{
    try {
        if constexpr (std::is_void_v<InvokeResult<F>>) {
            std::invoke(std::forward<F>(f));
            return {};
        } else {
            return std::invoke(std::forward<F>(f));
        }
    } catch (const Exception &e) {
        return Unex(E(std::invoke(std::forward<G>(map_error), e)));
    }
}

} // namespace detail

template <typename Exception, typename E, typename F, typename G>
    requires detail::SupportedErrorMapper<Exception, E, F, G>
[[nodiscard]] auto CatchException(F &&f, G &&map_error) -> Expected<detail::ExpectedValue<F>, E>
{
    return detail::CatchExceptionImpl<Exception, E>(std::forward<F>(f), std::forward<G>(map_error));
}

template <typename Exception, typename E, typename F>
    requires std::invocable<F> && std::copy_constructible<E>
[[nodiscard]] auto CatchException(F &&f, E error) -> Expected<detail::ExpectedValue<F>, E>
{
    return CatchException<Exception, E>(
        std::forward<F>(f), [error = std::move(error)](const Exception &) { return error; }
    );
}

template <typename E, typename F, typename G>
    requires detail::SupportedErrorMapper<std::exception, E, F, G>
[[nodiscard]] auto CatchUserver(F &&f, G &&map_error) -> Expected<detail::ExpectedValue<F>, E>
{
    return detail::CatchExceptionImpl<std::exception, E>(
        std::forward<F>(f), std::forward<G>(map_error)
    );
}

template <typename E, typename F>
    requires std::invocable<F> && std::copy_constructible<E>
[[nodiscard]] auto CatchUserver(F &&f, E error) -> Expected<detail::ExpectedValue<F>, E>
{
    return CatchException<std::exception, E>(std::forward<F>(f), std::move(error));
}

template <typename T, typename E, typename G>
    requires std::invocable<G, const ujson::Exception &> &&
             std::constructible_from<E, std::invoke_result_t<G, const ujson::Exception &>>
[[nodiscard]] Expected<T, E> Parse(const String &json_text, G &&map_error)
{
    return CatchException<ujson::Exception, E>(
        [&]() { return ujson::FromString(json_text.View()).template As<T>(); },
        std::forward<G>(map_error)
    );
}

template <typename T, typename E>
    requires std::copy_constructible<E>
[[nodiscard]] Expected<T, E> Parse(const String &json_text, E error)
{
    return CatchException<ujson::Exception, E>(
        [&]() { return ujson::FromString(json_text.View()).template As<T>(); }, std::move(error)
    );
}

template <typename T, typename E, typename G>
    requires std::invocable<G, const ujson::Exception &> &&
             std::constructible_from<E, std::invoke_result_t<G, const ujson::Exception &>>
[[nodiscard]] Expected<T, E> As(const ujson::Value &value, G &&map_error)
{
    return CatchException<ujson::Exception, E>(
        [&]() { return value.template As<T>(); }, std::forward<G>(map_error)
    );
}

template <typename T, typename E>
    requires std::copy_constructible<E>
[[nodiscard]] Expected<T, E> As(const ujson::Value &value, E error)
{
    return CatchException<ujson::Exception, E>(
        [&]() { return value.template As<T>(); }, std::move(error)
    );
}

template <typename T, typename E, typename G>
    requires std::invocable<G, const std::exception &> &&
             std::constructible_from<E, std::invoke_result_t<G, const std::exception &>>
[[nodiscard]] Expected<ujson::Value, E> ValueOf(const T &value, G &&map_error)
{
    return CatchUserver<E>(
        [&]() { return ujson::ValueBuilder(value).ExtractValue(); }, std::forward<G>(map_error)
    );
}

template <typename T, typename E>
    requires std::copy_constructible<E>
[[nodiscard]] Expected<ujson::Value, E> ValueOf(const T &value, E error)
{
    return CatchUserver<E>(
        [&]() { return ujson::ValueBuilder(value).ExtractValue(); }, std::move(error)
    );
}

template <typename E, typename G>
    requires std::invocable<G, const std::exception &> &&
             std::constructible_from<E, std::invoke_result_t<G, const std::exception &>>
[[nodiscard]] Expected<std::string, E> StringifyBytes(ujson::Value value, G &&map_error)
{
    return CatchUserver<E>(
        [&]() { return ujson::ToString(std::move(value)); }, std::forward<G>(map_error)
    );
}

template <typename E>
    requires std::copy_constructible<E>
[[nodiscard]] Expected<std::string, E> StringifyBytes(ujson::Value value, E error)
{
    return CatchUserver<E>([&]() { return ujson::ToString(std::move(value)); }, std::move(error));
}

template <typename T, typename E, typename G>
    requires std::copy_constructible<std::remove_cvref_t<G>> && (!detail::JsonTextInput<T>)
[[nodiscard]] Expected<std::string, E> StringifyBytes(const T &value, G &&map_error)
{
    auto mapper = std::forward<G>(map_error);
    return StringifyBytes<E>(TRY(ValueOf<T, E>(value, mapper)), mapper);
}

template <typename T, typename E>
    requires std::copy_constructible<E> && (!detail::JsonTextInput<T>)
[[nodiscard]] Expected<std::string, E> StringifyBytes(const T &value, E error)
{
    return StringifyBytes<E>(TRY(ValueOf<T, E>(value, error)), std::move(error));
}

template <typename E, typename G>
    requires std::invocable<G, const std::exception &> &&
             std::constructible_from<E, std::invoke_result_t<G, const std::exception &>>
[[nodiscard]] Expected<String, E> Stringify(ujson::Value value, G &&map_error)
{
    auto json_bytes = TRY(StringifyBytes<E>(std::move(value), std::forward<G>(map_error)));
    return String::FromBytes(json_bytes).Expect();
}

template <typename E>
    requires std::copy_constructible<E>
[[nodiscard]] Expected<String, E> Stringify(ujson::Value value, E error)
{
    auto json_bytes = TRY(StringifyBytes<E>(std::move(value), std::move(error)));
    return String::FromBytes(json_bytes).Expect();
}

template <typename T, typename E, typename G>
    requires std::copy_constructible<std::remove_cvref_t<G>> && (!detail::JsonTextInput<T>)
[[nodiscard]] Expected<String, E> Stringify(const T &value, G &&map_error)
{
    auto mapper = std::forward<G>(map_error);
    return Stringify<E>(TRY(ValueOf<T, E>(value, mapper)), mapper);
}

template <typename T, typename E>
    requires std::copy_constructible<E> && (!detail::JsonTextInput<T>)
[[nodiscard]] Expected<String, E> Stringify(const T &value, E error)
{
    return Stringify<E>(TRY(ValueOf<T, E>(value, error)), std::move(error));
}

} // namespace ws::json
