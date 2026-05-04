#pragma once

#include "grab_value.hpp"

#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace ws::detail {

template <typename T> struct IsOptional : std::false_type {};
template <typename T> struct IsOptional<std::optional<T>> : std::true_type {};

template <typename T>
struct IsTrySupported
    : std::bool_constant<IsExpected<RemoveCvref<T>>::value || IsOptional<RemoveCvref<T>>::value> {};

struct TryEmptyReturn final {
    template <typename T> constexpr operator std::optional<T>() const noexcept { return {}; }
};

template <typename E> struct TryExpectedFailure final {
    template <typename T> constexpr operator std::optional<T>() const noexcept { return {}; }

    template <typename T> [[nodiscard]] operator Expected<T, E>() & { return Unex(error); }

    template <typename T> [[nodiscard]] operator Expected<T, E>() const & { return Unex(error); }

    template <typename T> [[nodiscard]] operator Expected<T, E>() &&
    {
        return Unex(std::move(error));
    }

    E error;
};

template <typename T, typename E>
[[nodiscard]] constexpr bool TryHasValue(const Expected<T, E> &expected) noexcept
{
    return expected.HasValue();
}

template <typename T> [[nodiscard]] constexpr bool TryHasValue(const std::optional<T> &opt) noexcept
{
    return opt.has_value();
}

template <typename T, typename E> [[nodiscard]] inline Unex<E> TryAsUnex(Expected<T, E> &expected)
{
    return Unex(expected.Error());
}

template <typename T, typename E>
[[nodiscard]] inline Unex<E> TryAsUnex(const Expected<T, E> &expected)
{
    return Unex(expected.Error());
}

template <typename T, typename E> [[nodiscard]] inline Unex<E> TryAsUnex(Expected<T, E> &&expected)
{
    return Unex(std::move(expected).Error());
}

template <typename T, typename E> inline T TryExtract(Expected<T, E> &expected)
{
    return GrabValueOf(expected);
}

template <typename T, typename E> inline T TryExtract(Expected<T, E> &&expected)
{
    return GrabValueOf(std::move(expected));
}

template <typename E> inline void TryExtract(Expected<void, E> &expected) { expected.Value(); }

template <typename E> inline void TryExtract(Expected<void, E> &&expected)
{
    std::move(expected).Value();
}

template <typename T> inline T TryExtract(std::optional<T> &opt) { return GrabValueOf(opt); }

template <typename T> inline T TryExtract(std::optional<T> &&opt)
{
    return GrabValueOf(std::move(opt));
}

template <typename T> [[nodiscard]] inline auto TryFailure(T &&value)
{
    static_assert(IsTrySupported<T>::value, "TRY only supports ws::Expected and std::optional");

    if constexpr (IsExpected<RemoveCvref<T>>::value) {
        return TryExpectedFailure{std::forward<T>(value).Error()};
    } else {
        return TryEmptyReturn{};
    }
}

template <typename T, typename F> [[nodiscard]] constexpr auto TryMapError(T &&value, F &&map_error)
{
    static_assert(IsExpected<RemoveCvref<T>>::value, "TRY_MAP_ERR only supports ws::Expected");
    return std::forward<T>(value).TransformError(std::forward<F>(map_error));
}

template <typename T, typename F> [[nodiscard]] constexpr auto TryMap(T &&value, F &&map_value)
{
    static_assert(IsExpected<RemoveCvref<T>>::value, "TRY_MAP only supports ws::Expected");
    return std::forward<T>(value).Transform(std::forward<F>(map_value));
}

template <typename T, typename F>
[[nodiscard]] constexpr auto TryOkOrElse(T &&value, F &&make_error)
    -> Expected<typename RemoveCvref<T>::value_type, RemoveCvref<std::invoke_result_t<F>>>
{
    static_assert(IsOptional<RemoveCvref<T>>::value, "TRY_OK_OR_ELSE only supports std::optional");

    using Optional = RemoveCvref<T>;
    using Value = typename Optional::value_type;
    using Error = RemoveCvref<std::invoke_result_t<F>>;

    if (value)
        return Expected<Value, Error>{std::forward<T>(value).value()};
    return Expected<Value, Error>{Unex(std::invoke(std::forward<F>(make_error)))};
}

} // namespace ws::detail

#if defined(__clang__)
#define WS_TRY_DIAGNOSTIC_PUSH _Pragma("clang diagnostic push")
#define WS_TRY_DIAGNOSTIC_IGNORE                                                                   \
    _Pragma("clang diagnostic ignored \"-Wgnu-statement-expression-from-macro-expansion\"")        \
        _Pragma("clang diagnostic ignored \"-Wshadow\"")
#define WS_TRY_DIAGNOSTIC_POP _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
#define WS_TRY_DIAGNOSTIC_PUSH _Pragma("GCC diagnostic push")
#define WS_TRY_DIAGNOSTIC_IGNORE                                                                   \
    _Pragma("GCC diagnostic ignored \"-Wpedantic\"") _Pragma("GCC diagnostic ignored \"-Wshadow\"")
#define WS_TRY_DIAGNOSTIC_POP _Pragma("GCC diagnostic pop")
#else
#error "TRY requires compiler support for GNU statement expressions"
#endif

#ifdef TRY
#error "TRY is already defined"
#endif

#ifdef TRY_MAP_ERR
#error "TRY_MAP_ERR is already defined"
#endif

#ifdef TRY_ERR_AS
#error "TRY_ERR_AS is already defined"
#endif

#ifdef TRY_MAP
#error "TRY_MAP is already defined"
#endif

#ifdef TRY_OK_OR
#error "TRY_OK_OR is already defined"
#endif

#ifdef TRY_OK_OR_ELSE
#error "TRY_OK_OR_ELSE is already defined"
#endif

#ifdef ENSURE
#error "ENSURE is already defined"
#endif

#define TRY(...)                                                                                   \
    WS_TRY_DIAGNOSTIC_PUSH WS_TRY_DIAGNOSTIC_IGNORE({                                              \
        auto &&_temporaryTryResult = (__VA_ARGS__);                                                \
        static_assert(                                                                             \
            ::ws::detail::IsTrySupported<decltype(_temporaryTryResult)>::value,                    \
            "TRY only supports ws::Expected and std::optional"                                     \
        );                                                                                         \
        if (!::ws::detail::TryHasValue(_temporaryTryResult)) [[unlikely]] {                        \
            return ::ws::detail::TryFailure(                                                       \
                std::forward<decltype(_temporaryTryResult)>(_temporaryTryResult)                   \
            );                                                                                     \
        }                                                                                          \
        ::ws::detail::TryExtract(                                                                  \
            std::forward<decltype(_temporaryTryResult)>(_temporaryTryResult)                       \
        );                                                                                         \
    }) WS_TRY_DIAGNOSTIC_POP

#define TRY_MAP_ERR(expr, mapper) TRY(::ws::detail::TryMapError((expr), (mapper)))

#define TRY_ERR_AS(expr, err) TRY_MAP_ERR((expr), [&](auto &&) { return (err); })

#define TRY_MAP(expr, mapper) TRY(::ws::detail::TryMap((expr), (mapper)))

#define TRY_OK_OR(opt, err) TRY(::ws::detail::TryOkOrElse((opt), [&]() { return (err); }))

#define TRY_OK_OR_ELSE(opt, make_error) TRY(::ws::detail::TryOkOrElse((opt), (make_error)))

#define ENSURE(cond, err)                                                                          \
    do {                                                                                           \
        if (!(cond)) [[unlikely]]                                                                  \
            return ::ws::Unex((err));                                                              \
    } while (false)
