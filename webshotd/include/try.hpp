#pragma once

#include "grab_value.hpp"

#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace v1::detail {

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
[[nodiscard]] constexpr bool tryHasValue(const Expected<T, E> &expected) noexcept
{
    return expected.hasValue();
}

template <typename T> [[nodiscard]] constexpr bool tryHasValue(const std::optional<T> &opt) noexcept
{
    return opt.has_value();
}

template <typename T, typename E> [[nodiscard]] inline Unex<E> tryAsUnex(Expected<T, E> &expected)
{
    return Unex(expected.error());
}

template <typename T, typename E>
[[nodiscard]] inline Unex<E> tryAsUnex(const Expected<T, E> &expected)
{
    return Unex(expected.error());
}

template <typename T, typename E> [[nodiscard]] inline Unex<E> tryAsUnex(Expected<T, E> &&expected)
{
    return Unex(std::move(expected).error());
}

template <typename T, typename E> inline T tryExtract(Expected<T, E> &expected)
{
    return grabValueOf(expected);
}

template <typename T, typename E> inline T tryExtract(Expected<T, E> &&expected)
{
    return grabValueOf(std::move(expected));
}

template <typename E> inline void tryExtract(Expected<void, E> &expected) { expected.value(); }

template <typename E> inline void tryExtract(Expected<void, E> &&expected)
{
    std::move(expected).value();
}

template <typename T> inline T tryExtract(std::optional<T> &opt) { return grabValueOf(opt); }

template <typename T> inline T tryExtract(std::optional<T> &&opt)
{
    return grabValueOf(std::move(opt));
}

template <typename T> [[nodiscard]] inline auto tryFailure(T &&value)
{
    static_assert(IsTrySupported<T>::value, "TRY only supports v1::Expected and std::optional");

    if constexpr (IsExpected<RemoveCvref<T>>::value) {
        return TryExpectedFailure{std::forward<T>(value).error()};
    } else {
        return TryEmptyReturn{};
    }
}

template <typename T, typename F> [[nodiscard]] constexpr auto tryMapError(T &&value, F &&mapError)
{
    static_assert(IsExpected<RemoveCvref<T>>::value, "TRY_MAP_ERR only supports v1::Expected");
    return std::forward<T>(value).transformError(std::forward<F>(mapError));
}

template <typename T, typename F> [[nodiscard]] constexpr auto tryMap(T &&value, F &&mapValue)
{
    static_assert(IsExpected<RemoveCvref<T>>::value, "TRY_MAP only supports v1::Expected");
    return std::forward<T>(value).transform(std::forward<F>(mapValue));
}

template <typename T, typename F>
[[nodiscard]] constexpr auto tryOkOrElse(T &&value, F &&makeError)
    -> Expected<typename RemoveCvref<T>::value_type, RemoveCvref<std::invoke_result_t<F>>>
{
    static_assert(IsOptional<RemoveCvref<T>>::value, "TRY_OK_OR_ELSE only supports std::optional");

    using Optional = RemoveCvref<T>;
    using Value = typename Optional::value_type;
    using Error = RemoveCvref<std::invoke_result_t<F>>;

    if (value)
        return Expected<Value, Error>{std::forward<T>(value).value()};
    return Expected<Value, Error>{Unex(std::invoke(std::forward<F>(makeError)))};
}

} // namespace v1::detail

#if defined(__clang__)
#define V1_TRY_DIAGNOSTIC_PUSH _Pragma("clang diagnostic push")
#define V1_TRY_DIAGNOSTIC_IGNORE                                                                   \
    _Pragma("clang diagnostic ignored \"-Wgnu-statement-expression-from-macro-expansion\"")        \
        _Pragma("clang diagnostic ignored \"-Wshadow\"")
#define V1_TRY_DIAGNOSTIC_POP _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
#define V1_TRY_DIAGNOSTIC_PUSH _Pragma("GCC diagnostic push")
#define V1_TRY_DIAGNOSTIC_IGNORE                                                                   \
    _Pragma("GCC diagnostic ignored \"-Wpedantic\"") _Pragma("GCC diagnostic ignored \"-Wshadow\"")
#define V1_TRY_DIAGNOSTIC_POP _Pragma("GCC diagnostic pop")
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
    V1_TRY_DIAGNOSTIC_PUSH V1_TRY_DIAGNOSTIC_IGNORE({                                              \
        auto &&_temporaryTryResult = (__VA_ARGS__);                                                \
        static_assert(                                                                             \
            ::v1::detail::IsTrySupported<decltype(_temporaryTryResult)>::value,                    \
            "TRY only supports v1::Expected and std::optional"                                     \
        );                                                                                         \
        if (!::v1::detail::tryHasValue(_temporaryTryResult)) [[unlikely]] {                        \
            return ::v1::detail::tryFailure(                                                       \
                std::forward<decltype(_temporaryTryResult)>(_temporaryTryResult)                   \
            );                                                                                     \
        }                                                                                          \
        ::v1::detail::tryExtract(                                                                  \
            std::forward<decltype(_temporaryTryResult)>(_temporaryTryResult)                       \
        );                                                                                         \
    }) V1_TRY_DIAGNOSTIC_POP

#define TRY_MAP_ERR(expr, mapper) TRY(::v1::detail::tryMapError((expr), (mapper)))

#define TRY_ERR_AS(expr, err) TRY_MAP_ERR((expr), [&](auto &&) { return (err); })

#define TRY_MAP(expr, mapper) TRY(::v1::detail::tryMap((expr), (mapper)))

#define TRY_OK_OR(opt, err) TRY(::v1::detail::tryOkOrElse((opt), [&]() { return (err); }))

#define TRY_OK_OR_ELSE(opt, makeError) TRY(::v1::detail::tryOkOrElse((opt), (makeError)))

#define ENSURE(cond, err)                                                                          \
    do {                                                                                           \
        if (!(cond)) [[unlikely]]                                                                  \
            return ::v1::Unex((err));                                                              \
    } while (false)
