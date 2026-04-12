#pragma once

#include <concepts>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <userver/utils/assert.hpp>

namespace v1 {

template <typename T, typename E> class Expected;

namespace detail {

template <typename X> struct IsExpected : std::false_type {};
template <typename T, typename E> struct IsExpected<Expected<T, E>> : std::true_type {};

template <typename T> using RemoveCvref = std::remove_cvref_t<T>;

template <typename U, typename T>
concept SameUncvref = std::same_as<RemoveCvref<U>, T>;

[[noreturn]] inline void abortExpected(std::string_view message) noexcept
{
    userver::utils::AbortWithStacktrace(std::string(message));
}

template <typename E>
[[noreturn]] inline void abortUnexpectedValue(std::string_view where, const E &error) noexcept
{
    static_cast<void>(error);
    abortExpected(std::string(where) + ": expected value");
}

[[noreturn]] inline void abortUnexpectedError(std::string_view where) noexcept
{
    abortExpected(std::string(where) + ": expected error");
}

} // namespace detail

template <typename T, typename E> class [[nodiscard]] Expected final {
public:
    using value_type = T;
    using error_type = E;
    using StdExpected = std::expected<T, E>;
    using Self = Expected<T, E>;

    constexpr Expected() = default;

    constexpr Expected(const Expected &) = default;
    constexpr Expected(Expected &&) noexcept = default;
    constexpr Expected &operator=(const Expected &) = default;
    constexpr Expected &operator=(Expected &&) noexcept = default;
    constexpr ~Expected() = default;

    constexpr Expected(const T &inner) : inner(inner) {}
    constexpr Expected(T &&inner) noexcept(std::is_nothrow_move_constructible_v<T>)
        : inner(std::move(inner))
    {
    }

    template <typename U>
        requires(
            !detail::SameUncvref<U, T> && !detail::SameUncvref<U, Self> &&
            !detail::SameUncvref<U, StdExpected> && !detail::SameUncvref<U, std::unexpected<E>> &&
            std::constructible_from<T, U>
        )
    constexpr Expected(U &&value) noexcept(std::is_nothrow_constructible_v<T, U>)
        : inner(T(std::forward<U>(value)))
    {
    }

    constexpr Expected(std::unexpected<E> inner) noexcept(std::is_nothrow_move_constructible_v<E>)
        : inner(std::move(inner))
    {
    }

    constexpr Expected(const StdExpected &inner) : inner(inner) {}
    constexpr Expected(
        StdExpected &&inner
    ) noexcept(std::is_nothrow_move_constructible_v<StdExpected>)
        : inner(std::move(inner))
    {
    }

    [[nodiscard]] constexpr bool hasValue() const noexcept { return inner.has_value(); }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] constexpr T &value() & noexcept
    {
        if (!inner)
            detail::abortUnexpectedValue("value()", inner.error());
        return inner.value();
    }

    [[nodiscard]] constexpr const T &value() const & noexcept
    {
        if (!inner)
            detail::abortUnexpectedValue("value() const", inner.error());
        return inner.value();
    }

    [[nodiscard]] constexpr T &&value() && noexcept
    {
        if (!inner)
            detail::abortUnexpectedValue("value() &&", inner.error());
        return std::move(inner.value());
    }

    [[nodiscard]] constexpr E &error() & noexcept
    {
        if (inner)
            detail::abortUnexpectedError("error()");
        return inner.error();
    }

    [[nodiscard]] constexpr const E &error() const & noexcept
    {
        if (inner)
            detail::abortUnexpectedError("error() const");
        return inner.error();
    }

    [[nodiscard]] constexpr E &&error() && noexcept
    {
        if (inner)
            detail::abortUnexpectedError("error() &&");
        return std::move(inner.error());
    }

    [[nodiscard]] constexpr T *operator->() noexcept { return &value(); }
    [[nodiscard]] constexpr const T *operator->() const noexcept { return &value(); }
    [[nodiscard]] constexpr T &operator*() & noexcept { return value(); }
    [[nodiscard]] constexpr const T &operator*() const & noexcept { return value(); }

    [[nodiscard]] constexpr T &expect(std::string_view message) & noexcept
    {
        if (!inner) {
            detail::abortExpected(message);
        }
        return inner.value();
    }

    [[nodiscard]] constexpr const T &expect(std::string_view message) const & noexcept
    {
        if (!inner) {
            detail::abortExpected(message);
        }
        return inner.value();
    }

    [[nodiscard]] constexpr T &expect() & noexcept { return expect("Expected::expect() failed"); }
    [[nodiscard]] constexpr const T &expect() const & noexcept
    {
        return expect("Expected::expect() failed");
    }

    template <typename U>
        requires std::constructible_from<T, U>
    [[nodiscard]] constexpr T valueOr(U &&defaultValue) const &
    {
        if (inner)
            return inner.value();
        return T(std::forward<U>(defaultValue));
    }

    template <typename U>
        requires std::constructible_from<T, U>
    [[nodiscard]] constexpr T valueOr(U &&defaultValue) &&
    {
        if (inner)
            return std::move(inner.value());
        return T(std::forward<U>(defaultValue));
    }

    template <typename F>
        requires std::invocable<F, T &>
    [[nodiscard]] constexpr auto
    transform(F &&f) & -> Expected<std::remove_cvref_t<std::invoke_result_t<F, T &>>, E>
    {
        using U = std::remove_cvref_t<std::invoke_result_t<F, T &>>;
        if (!inner)
            return std::unexpected(inner.error());
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<F>(f), inner.value());
            return Expected<void, E>{};
        } else {
            return Expected<U, E>{std::invoke(std::forward<F>(f), inner.value())};
        }
    }

    template <typename F>
        requires std::invocable<F, const T &>
    [[nodiscard]] constexpr auto
    transform(F &&f) const & -> Expected<std::remove_cvref_t<std::invoke_result_t<F, const T &>>, E>
    {
        using U = std::remove_cvref_t<std::invoke_result_t<F, const T &>>;
        if (!inner)
            return std::unexpected(inner.error());
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<F>(f), inner.value());
            return Expected<void, E>{};
        } else {
            return Expected<U, E>{std::invoke(std::forward<F>(f), inner.value())};
        }
    }

    template <typename F>
        requires std::invocable<F, T &&>
    [[nodiscard]] constexpr auto
    transform(F &&f) && -> Expected<std::remove_cvref_t<std::invoke_result_t<F, T &&>>, E>
    {
        using U = std::remove_cvref_t<std::invoke_result_t<F, T &&>>;
        if (!inner)
            return std::unexpected(std::move(inner.error()));
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<F>(f), std::move(inner.value()));
            return Expected<void, E>{};
        } else {
            return Expected<U, E>{std::invoke(std::forward<F>(f), std::move(inner.value()))};
        }
    }

    template <typename F>
        requires std::invocable<F, T &> &&
                 detail::IsExpected<std::remove_cvref_t<std::invoke_result_t<F, T &>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, T &>>::error_type, E>
    [[nodiscard]] constexpr auto
    andThen(F &&f) & -> std::remove_cvref_t<std::invoke_result_t<F, T &>>
    {
        if (!inner)
            return std::unexpected(inner.error());
        return std::invoke(std::forward<F>(f), inner.value());
    }

    template <typename F>
        requires std::invocable<F, const T &> &&
                 detail::IsExpected<
                     std::remove_cvref_t<std::invoke_result_t<F, const T &>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, const T &>>::error_type,
                     E>
    [[nodiscard]] constexpr auto
    andThen(F &&f) const & -> std::remove_cvref_t<std::invoke_result_t<F, const T &>>
    {
        if (!inner)
            return std::unexpected(inner.error());
        return std::invoke(std::forward<F>(f), inner.value());
    }

    template <typename F>
        requires std::invocable<F, T &&> &&
                 detail::IsExpected<std::remove_cvref_t<std::invoke_result_t<F, T &&>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, T &&>>::error_type, E>
    [[nodiscard]] constexpr auto
    andThen(F &&f) && -> std::remove_cvref_t<std::invoke_result_t<F, T &&>>
    {
        if (!inner)
            return std::unexpected(std::move(inner.error()));
        return std::invoke(std::forward<F>(f), std::move(inner.value()));
    }

    template <typename F>
        requires std::invocable<F, E &> &&
                 detail::IsExpected<std::remove_cvref_t<std::invoke_result_t<F, E &>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, E &>>::value_type, T>
    [[nodiscard]] constexpr auto
    orElse(F &&f) & -> std::remove_cvref_t<std::invoke_result_t<F, E &>>
    {
        using R = std::remove_cvref_t<std::invoke_result_t<F, E &>>;
        if (inner)
            return R(inner);
        return std::invoke(std::forward<F>(f), inner.error());
    }

    template <typename F>
        requires std::invocable<F, const E &> &&
                 detail::IsExpected<
                     std::remove_cvref_t<std::invoke_result_t<F, const E &>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, const E &>>::value_type,
                     T>
    [[nodiscard]] constexpr auto
    orElse(F &&f) const & -> std::remove_cvref_t<std::invoke_result_t<F, const E &>>
    {
        using R = std::remove_cvref_t<std::invoke_result_t<F, const E &>>;
        if (inner)
            return R(inner);
        return std::invoke(std::forward<F>(f), inner.error());
    }

    template <typename F>
        requires std::invocable<F, E &&> &&
                 detail::IsExpected<std::remove_cvref_t<std::invoke_result_t<F, E &&>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, E &&>>::value_type, T>
    [[nodiscard]] constexpr auto
    orElse(F &&f) && -> std::remove_cvref_t<std::invoke_result_t<F, E &&>>
    {
        using R = std::remove_cvref_t<std::invoke_result_t<F, E &&>>;
        if (inner)
            return R(std::move(inner));
        return std::invoke(std::forward<F>(f), std::move(inner.error()));
    }

    template <typename F>
        requires std::invocable<F, E &>
    [[nodiscard]] constexpr auto
    transformError(F &&f) & -> Expected<T, std::remove_cvref_t<std::invoke_result_t<F, E &>>>
    {
        using E2 = std::remove_cvref_t<std::invoke_result_t<F, E &>>;
        if (inner)
            return Expected<T, E2>{inner.value()};
        return std::unexpected(std::invoke(std::forward<F>(f), inner.error()));
    }

    template <typename F>
        requires std::invocable<F, const E &>
    [[nodiscard]] constexpr auto transformError(
        F &&f
    ) const & -> Expected<T, std::remove_cvref_t<std::invoke_result_t<F, const E &>>>
    {
        using E2 = std::remove_cvref_t<std::invoke_result_t<F, const E &>>;
        if (inner)
            return Expected<T, E2>{inner.value()};
        return std::unexpected(std::invoke(std::forward<F>(f), inner.error()));
    }

    template <typename F>
        requires std::invocable<F, E &&>
    [[nodiscard]] constexpr auto
    transformError(F &&f) && -> Expected<T, std::remove_cvref_t<std::invoke_result_t<F, E &&>>>
    {
        using E2 = std::remove_cvref_t<std::invoke_result_t<F, E &&>>;
        if (inner)
            return Expected<T, E2>{std::move(inner.value())};
        return std::unexpected(std::invoke(std::forward<F>(f), std::move(inner.error())));
    }

private:
    StdExpected inner;
};

template <typename E> class [[nodiscard]] Expected<void, E> final {
public:
    using value_type = void;
    using error_type = E;
    using StdExpected = std::expected<void, E>;

    constexpr Expected() = default;
    constexpr Expected(const Expected &) = default;
    constexpr Expected(Expected &&) noexcept = default;
    constexpr Expected &operator=(const Expected &) = default;
    constexpr Expected &operator=(Expected &&) noexcept = default;
    constexpr ~Expected() = default;

    constexpr Expected(std::unexpected<E> inner) noexcept(std::is_nothrow_move_constructible_v<E>)
        : inner(std::move(inner))
    {
    }

    constexpr Expected(const StdExpected &inner) : inner(inner) {}
    constexpr Expected(
        StdExpected &&inner
    ) noexcept(std::is_nothrow_move_constructible_v<StdExpected>)
        : inner(std::move(inner))
    {
    }

    [[nodiscard]] constexpr bool hasValue() const noexcept { return inner.has_value(); }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return hasValue(); }

    constexpr void value() const noexcept
    {
        if (!inner)
            detail::abortUnexpectedValue("value() void", inner.error());
    }

    [[nodiscard]] constexpr E &error() & noexcept
    {
        if (inner)
            detail::abortUnexpectedError("error() void");
        return inner.error();
    }

    [[nodiscard]] constexpr const E &error() const & noexcept
    {
        if (inner)
            detail::abortUnexpectedError("error() void const");
        return inner.error();
    }

    constexpr void expect(std::string_view message) const noexcept
    {
        if (!inner) {
            detail::abortExpected(message);
        }
    }

    constexpr void expect() const noexcept { expect("Expected::expect() failed"); }

    template <typename F>
        requires std::invocable<F>
    [[nodiscard]] constexpr auto transform(F &&f) const
        -> Expected<std::remove_cvref_t<std::invoke_result_t<F>>, E>
    {
        using U = std::remove_cvref_t<std::invoke_result_t<F>>;
        if (!inner)
            return std::unexpected(inner.error());
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<F>(f));
            return Expected<void, E>{};
        } else {
            return Expected<U, E>{std::invoke(std::forward<F>(f))};
        }
    }

    template <typename F>
        requires std::invocable<F> &&
                 detail::IsExpected<std::remove_cvref_t<std::invoke_result_t<F>>>::value &&
                 std::same_as<typename std::remove_cvref_t<std::invoke_result_t<F>>::error_type, E>
    [[nodiscard]] constexpr auto andThen(F &&f) const
        -> std::remove_cvref_t<std::invoke_result_t<F>>
    {
        if (!inner)
            return std::unexpected(inner.error());
        return std::invoke(std::forward<F>(f));
    }

    template <typename F>
        requires std::invocable<F, E &> &&
                 detail::IsExpected<std::remove_cvref_t<std::invoke_result_t<F, E &>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, E &>>::value_type, void>
    [[nodiscard]] constexpr auto
    orElse(F &&f) & -> std::remove_cvref_t<std::invoke_result_t<F, E &>>
    {
        using R = std::remove_cvref_t<std::invoke_result_t<F, E &>>;
        if (inner)
            return R(inner);
        return std::invoke(std::forward<F>(f), inner.error());
    }

    template <typename F>
        requires std::invocable<F, const E &> &&
                 detail::IsExpected<
                     std::remove_cvref_t<std::invoke_result_t<F, const E &>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, const E &>>::value_type,
                     void>
    [[nodiscard]] constexpr auto
    orElse(F &&f) const & -> std::remove_cvref_t<std::invoke_result_t<F, const E &>>
    {
        using R = std::remove_cvref_t<std::invoke_result_t<F, const E &>>;
        if (inner)
            return R(inner);
        return std::invoke(std::forward<F>(f), inner.error());
    }

    template <typename F>
        requires std::invocable<F, E &&> &&
                 detail::IsExpected<std::remove_cvref_t<std::invoke_result_t<F, E &&>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, E &&>>::value_type, void>
    [[nodiscard]] constexpr auto
    orElse(F &&f) && -> std::remove_cvref_t<std::invoke_result_t<F, E &&>>
    {
        using R = std::remove_cvref_t<std::invoke_result_t<F, E &&>>;
        if (inner)
            return R(std::move(inner));
        return std::invoke(std::forward<F>(f), std::move(inner.error()));
    }

    template <typename F>
        requires std::invocable<F, E &>
    [[nodiscard]] constexpr auto
    transformError(F &&f) & -> Expected<void, std::remove_cvref_t<std::invoke_result_t<F, E &>>>
    {
        using E2 = std::remove_cvref_t<std::invoke_result_t<F, E &>>;
        if (inner)
            return Expected<void, E2>{};
        return std::unexpected(std::invoke(std::forward<F>(f), inner.error()));
    }

    template <typename F>
        requires std::invocable<F, const E &>
    [[nodiscard]] constexpr auto transformError(
        F &&f
    ) const & -> Expected<void, std::remove_cvref_t<std::invoke_result_t<F, const E &>>>
    {
        using E2 = std::remove_cvref_t<std::invoke_result_t<F, const E &>>;
        if (inner)
            return Expected<void, E2>{};
        return std::unexpected(std::invoke(std::forward<F>(f), inner.error()));
    }

    template <typename F>
        requires std::invocable<F, E &&>
    [[nodiscard]] constexpr auto
    transformError(F &&f) && -> Expected<void, std::remove_cvref_t<std::invoke_result_t<F, E &&>>>
    {
        using E2 = std::remove_cvref_t<std::invoke_result_t<F, E &&>>;
        if (inner)
            return Expected<void, E2>{};
        return std::unexpected(std::invoke(std::forward<F>(f), std::move(inner.error())));
    }

private:
    StdExpected inner;
};

} // namespace v1
