#pragma once

#include <array>
#include <concepts>
#include <cstdio>
#include <expected>
#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>

#include <userver/utils/assert.hpp>

namespace ws {

namespace us = userver;
template <typename T, typename E> class Expected;
template <typename E> class Unex;

namespace detail {

template <typename X> struct IsExpected : std::false_type {};
template <typename T, typename E> struct IsExpected<Expected<T, E>> : std::true_type {};

template <typename T> using RemoveCvref = std::remove_cvref_t<T>;

template <typename U, typename T>
concept SameUncvref = std::same_as<RemoveCvref<U>, T>;

[[noreturn]] inline void AbortExpected(std::string_view message) noexcept
{
    us::utils::AbortWithStacktrace(message);
}

template <size_t N>
[[noreturn]] inline void AbortExpectedAt(std::string_view where, const char (&suffix)[N]) noexcept
{
    std::array<char, 128> buf{};
    const int written = std::snprintf(
        buf.data(), buf.size(), "%.*s: %s", static_cast<int>(where.size()), where.data(), suffix
    );

    if (written > 0) {
        const size_t len = static_cast<size_t>(written) < buf.size() ? static_cast<size_t>(written)
                                                                     : (buf.size() - 1);
        us::utils::AbortWithStacktrace(std::string_view{buf.data(), len});
    }

    AbortExpected(suffix);
}

template <typename E>
[[noreturn]] inline void AbortUnexpectedValue(std::string_view where, const E &error) noexcept
{
    static_cast<void>(error);
    AbortExpectedAt(where, "expected value");
}

[[noreturn]] inline void AbortUnexpectedError(std::string_view where) noexcept
{
    AbortExpectedAt(where, "expected error");
}

} // namespace detail

template <typename E> class [[nodiscard]] Unex final {
public:
    using error_type = E;

    constexpr Unex(const Unex &) = default;
    constexpr Unex(Unex &&) noexcept = default;
    constexpr Unex &operator=(const Unex &) = default;
    constexpr Unex &operator=(Unex &&) noexcept = default;
    constexpr ~Unex() = default;

    constexpr explicit Unex(const E &inner) : inner_(inner) {}
    constexpr explicit Unex(E &&inner) noexcept(std::is_nothrow_move_constructible_v<E>)
        : inner_(std::move(inner))
    {
    }

    template <typename U>
        requires(
            !detail::SameUncvref<U, E> && !detail::SameUncvref<U, Unex> &&
            std::constructible_from<E, U>
        )
    constexpr explicit Unex(U &&value) noexcept(std::is_nothrow_constructible_v<E, U>)
        : inner_(E(std::forward<U>(value)))
    {
    }

    [[nodiscard]] constexpr E &Error() & noexcept { return inner_; }
    [[nodiscard]] constexpr const E &Error() const & noexcept { return inner_; }
    [[nodiscard]] constexpr E &&Error() && noexcept { return std::move(inner_); }

private:
    E inner_;
};

template <typename E> Unex(E) -> Unex<detail::RemoveCvref<E>>;

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

    constexpr Expected(const T &inner) : inner_(inner) {}
    constexpr Expected(T &&inner) noexcept(std::is_nothrow_move_constructible_v<T>)
        : inner_(std::move(inner))
    {
    }

    template <typename U>
        requires(
            !detail::SameUncvref<U, T> && !detail::SameUncvref<U, Self> &&
            !detail::SameUncvref<U, StdExpected> && !detail::SameUncvref<U, std::unexpected<E>> &&
            !detail::SameUncvref<U, Unex<E>> && std::constructible_from<T, U>
        )
    constexpr Expected(U &&value) noexcept(std::is_nothrow_constructible_v<T, U>)
        : inner_(T(std::forward<U>(value)))
    {
    }

    template <typename U>
        requires std::constructible_from<E, const U &>
    constexpr Expected(const Unex<U> &unex) : inner_(std::unexpected<E>(E(unex.Error())))
    {
    }

    template <typename U>
        requires std::constructible_from<E, U &&>
    constexpr Expected(Unex<U> &&unex) noexcept(std::is_nothrow_constructible_v<E, U &&>)
        : inner_(std::unexpected<E>(E(std::move(unex).Error())))
    {
    }

    constexpr Expected(std::unexpected<E> inner) noexcept(std::is_nothrow_move_constructible_v<E>)
        : inner_(std::move(inner))
    {
    }

    constexpr Expected(const StdExpected &inner) : inner_(inner) {}
    constexpr Expected(
        StdExpected &&inner
    ) noexcept(std::is_nothrow_move_constructible_v<StdExpected>)
        : inner_(std::move(inner))
    {
    }

    [[nodiscard]] constexpr bool HasValue() const noexcept { return inner_.has_value(); }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return HasValue(); }

    [[nodiscard]] constexpr T &Value() & noexcept
    {
        if (!inner_)
            detail::AbortUnexpectedValue("value()", inner_.error());
        return *inner_;
    }

    [[nodiscard]] constexpr const T &Value() const & noexcept
    {
        if (!inner_)
            detail::AbortUnexpectedValue("value() const", inner_.error());
        return *inner_;
    }

    [[nodiscard]] constexpr T &&Value() && noexcept
    {
        if (!inner_)
            detail::AbortUnexpectedValue("value() &&", inner_.error());
        return std::move(*inner_);
    }

    [[nodiscard]] constexpr E &Error() & noexcept
    {
        if (inner_)
            detail::AbortUnexpectedError("error()");
        return inner_.error();
    }

    [[nodiscard]] constexpr const E &Error() const & noexcept
    {
        if (inner_)
            detail::AbortUnexpectedError("error() const");
        return inner_.error();
    }

    [[nodiscard]] constexpr E &&Error() && noexcept
    {
        if (inner_)
            detail::AbortUnexpectedError("error() &&");
        return std::move(inner_.error());
    }

    [[nodiscard]] constexpr T *operator->() noexcept { return &Value(); }
    [[nodiscard]] constexpr const T *operator->() const noexcept { return &Value(); }
    [[nodiscard]] constexpr T &operator*() & noexcept { return Value(); }
    [[nodiscard]] constexpr const T &operator*() const & noexcept { return Value(); }

    [[nodiscard]] constexpr T &Expect(std::string_view message) & noexcept
    {
        if (!inner_) {
            detail::AbortExpected(message);
        }
        return *inner_;
    }

    [[nodiscard]] constexpr const T &Expect(std::string_view message) const & noexcept
    {
        if (!inner_) {
            detail::AbortExpected(message);
        }
        return *inner_;
    }

    [[nodiscard]] constexpr T &Expect() & noexcept { return Expect("Expected::expect() failed"); }
    [[nodiscard]] constexpr const T &Expect() const & noexcept
    {
        return Expect("Expected::expect() failed");
    }

    template <typename U>
        requires std::constructible_from<T, U>
    [[nodiscard]] constexpr T ValueOr(U &&default_value) const &
    {
        if (inner_)
            return *inner_;
        return T(std::forward<U>(default_value));
    }

    template <typename U>
        requires std::constructible_from<T, U>
    [[nodiscard]] constexpr T ValueOr(U &&default_value) &&
    {
        if (inner_)
            return std::move(*inner_);
        return T(std::forward<U>(default_value));
    }

    template <typename F>
        requires std::invocable<F, T &>
    [[nodiscard]] constexpr auto
    Transform(F &&f) & -> Expected<std::remove_cvref_t<std::invoke_result_t<F, T &>>, E>
    {
        using U = std::remove_cvref_t<std::invoke_result_t<F, T &>>;
        if (!inner_)
            return Unex(inner_.error());
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<F>(f), *inner_);
            return Expected<void, E>{};
        } else {
            return Expected<U, E>{std::invoke(std::forward<F>(f), *inner_)};
        }
    }

    template <typename F>
        requires std::invocable<F, const T &>
    [[nodiscard]] constexpr auto
    Transform(F &&f) const & -> Expected<std::remove_cvref_t<std::invoke_result_t<F, const T &>>, E>
    {
        using U = std::remove_cvref_t<std::invoke_result_t<F, const T &>>;
        if (!inner_)
            return Unex(inner_.error());
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<F>(f), *inner_);
            return Expected<void, E>{};
        } else {
            return Expected<U, E>{std::invoke(std::forward<F>(f), *inner_)};
        }
    }

    template <typename F>
        requires std::invocable<F, T &&>
    [[nodiscard]] constexpr auto
    Transform(F &&f) && -> Expected<std::remove_cvref_t<std::invoke_result_t<F, T &&>>, E>
    {
        using U = std::remove_cvref_t<std::invoke_result_t<F, T &&>>;
        if (!inner_)
            return Unex(std::move(inner_.error()));
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<F>(f), std::move(*inner_));
            return Expected<void, E>{};
        } else {
            return Expected<U, E>{std::invoke(std::forward<F>(f), std::move(*inner_))};
        }
    }

    template <typename F>
        requires std::invocable<F, T &> &&
                 detail::IsExpected<std::remove_cvref_t<std::invoke_result_t<F, T &>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, T &>>::error_type, E>
    [[nodiscard]] constexpr auto
    AndThen(F &&f) & -> std::remove_cvref_t<std::invoke_result_t<F, T &>>
    {
        if (!inner_)
            return Unex(inner_.error());
        return std::invoke(std::forward<F>(f), *inner_);
    }

    template <typename F>
        requires std::invocable<F, const T &> &&
                 detail::IsExpected<
                     std::remove_cvref_t<std::invoke_result_t<F, const T &>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, const T &>>::error_type,
                     E>
    [[nodiscard]] constexpr auto
    AndThen(F &&f) const & -> std::remove_cvref_t<std::invoke_result_t<F, const T &>>
    {
        if (!inner_)
            return Unex(inner_.error());
        return std::invoke(std::forward<F>(f), *inner_);
    }

    template <typename F>
        requires std::invocable<F, T &&> &&
                 detail::IsExpected<std::remove_cvref_t<std::invoke_result_t<F, T &&>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, T &&>>::error_type, E>
    [[nodiscard]] constexpr auto
    AndThen(F &&f) && -> std::remove_cvref_t<std::invoke_result_t<F, T &&>>
    {
        if (!inner_)
            return Unex(std::move(inner_.error()));
        return std::invoke(std::forward<F>(f), std::move(*inner_));
    }

    template <typename F>
        requires std::invocable<F, E &> &&
                 detail::IsExpected<std::remove_cvref_t<std::invoke_result_t<F, E &>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, E &>>::value_type, T>
    [[nodiscard]] constexpr auto
    OrElse(F &&f) & -> std::remove_cvref_t<std::invoke_result_t<F, E &>>
    {
        using R = std::remove_cvref_t<std::invoke_result_t<F, E &>>;
        if (inner_)
            return R(inner_);
        return std::invoke(std::forward<F>(f), inner_.error());
    }

    template <typename F>
        requires std::invocable<F, const E &> &&
                 detail::IsExpected<
                     std::remove_cvref_t<std::invoke_result_t<F, const E &>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, const E &>>::value_type,
                     T>
    [[nodiscard]] constexpr auto
    OrElse(F &&f) const & -> std::remove_cvref_t<std::invoke_result_t<F, const E &>>
    {
        using R = std::remove_cvref_t<std::invoke_result_t<F, const E &>>;
        if (inner_)
            return R(inner_);
        return std::invoke(std::forward<F>(f), inner_.error());
    }

    template <typename F>
        requires std::invocable<F, E &&> &&
                 detail::IsExpected<std::remove_cvref_t<std::invoke_result_t<F, E &&>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, E &&>>::value_type, T>
    [[nodiscard]] constexpr auto
    OrElse(F &&f) && -> std::remove_cvref_t<std::invoke_result_t<F, E &&>>
    {
        using R = std::remove_cvref_t<std::invoke_result_t<F, E &&>>;
        if (inner_)
            return R(std::move(inner_));
        return std::invoke(std::forward<F>(f), std::move(inner_.error()));
    }

    template <typename F>
        requires std::invocable<F, E &>
    [[nodiscard]] constexpr auto
    TransformError(F &&f) & -> Expected<T, std::remove_cvref_t<std::invoke_result_t<F, E &>>>
    {
        using E2 = std::remove_cvref_t<std::invoke_result_t<F, E &>>;
        if (inner_)
            return Expected<T, E2>{*inner_};
        return Unex(std::invoke(std::forward<F>(f), inner_.error()));
    }

    template <typename F>
        requires std::invocable<F, const E &>
    [[nodiscard]] constexpr auto TransformError(
        F &&f
    ) const & -> Expected<T, std::remove_cvref_t<std::invoke_result_t<F, const E &>>>
    {
        using E2 = std::remove_cvref_t<std::invoke_result_t<F, const E &>>;
        if (inner_)
            return Expected<T, E2>{*inner_};
        return Unex(std::invoke(std::forward<F>(f), inner_.error()));
    }

    template <typename F>
        requires std::invocable<F, E &&>
    [[nodiscard]] constexpr auto
    TransformError(F &&f) && -> Expected<T, std::remove_cvref_t<std::invoke_result_t<F, E &&>>>
    {
        using E2 = std::remove_cvref_t<std::invoke_result_t<F, E &&>>;
        if (inner_)
            return Expected<T, E2>{std::move(*inner_)};
        return Unex(std::invoke(std::forward<F>(f), std::move(inner_.error())));
    }

private:
    StdExpected inner_;
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

    template <typename U>
        requires std::constructible_from<E, const U &>
    constexpr Expected(const Unex<U> &unex) : inner_(std::unexpected<E>(E(unex.Error())))
    {
    }

    template <typename U>
        requires std::constructible_from<E, U &&>
    constexpr Expected(Unex<U> &&unex) noexcept(std::is_nothrow_constructible_v<E, U &&>)
        : inner_(std::unexpected<E>(E(std::move(unex).Error())))
    {
    }

    constexpr Expected(std::unexpected<E> inner) noexcept(std::is_nothrow_move_constructible_v<E>)
        : inner_(std::move(inner))
    {
    }

    constexpr Expected(const StdExpected &inner) : inner_(inner) {}
    constexpr Expected(
        StdExpected &&inner
    ) noexcept(std::is_nothrow_move_constructible_v<StdExpected>)
        : inner_(std::move(inner))
    {
    }

    [[nodiscard]] constexpr bool HasValue() const noexcept { return inner_.has_value(); }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return HasValue(); }

    constexpr void Value() const noexcept
    {
        if (!inner_)
            detail::AbortUnexpectedValue("value() void", inner_.error());
    }

    [[nodiscard]] constexpr E &Error() & noexcept
    {
        if (inner_)
            detail::AbortUnexpectedError("error() void");
        return inner_.error();
    }

    [[nodiscard]] constexpr const E &Error() const & noexcept
    {
        if (inner_)
            detail::AbortUnexpectedError("error() void const");
        return inner_.error();
    }

    constexpr void Expect(std::string_view message) const noexcept
    {
        if (!inner_) {
            detail::AbortExpected(message);
        }
    }

    constexpr void Expect() const noexcept { Expect("Expected::expect() failed"); }

    template <typename F>
        requires std::invocable<F>
    [[nodiscard]] constexpr auto Transform(F &&f) const
        -> Expected<std::remove_cvref_t<std::invoke_result_t<F>>, E>
    {
        using U = std::remove_cvref_t<std::invoke_result_t<F>>;
        if (!inner_)
            return Unex(inner_.error());
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
    [[nodiscard]] constexpr auto AndThen(F &&f) const
        -> std::remove_cvref_t<std::invoke_result_t<F>>
    {
        if (!inner_)
            return Unex(inner_.error());
        return std::invoke(std::forward<F>(f));
    }

    template <typename F>
        requires std::invocable<F, E &> &&
                 detail::IsExpected<std::remove_cvref_t<std::invoke_result_t<F, E &>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, E &>>::value_type, void>
    [[nodiscard]] constexpr auto
    OrElse(F &&f) & -> std::remove_cvref_t<std::invoke_result_t<F, E &>>
    {
        using R = std::remove_cvref_t<std::invoke_result_t<F, E &>>;
        if (inner_)
            return R(inner_);
        return std::invoke(std::forward<F>(f), inner_.error());
    }

    template <typename F>
        requires std::invocable<F, const E &> &&
                 detail::IsExpected<
                     std::remove_cvref_t<std::invoke_result_t<F, const E &>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, const E &>>::value_type,
                     void>
    [[nodiscard]] constexpr auto
    OrElse(F &&f) const & -> std::remove_cvref_t<std::invoke_result_t<F, const E &>>
    {
        using R = std::remove_cvref_t<std::invoke_result_t<F, const E &>>;
        if (inner_)
            return R(inner_);
        return std::invoke(std::forward<F>(f), inner_.error());
    }

    template <typename F>
        requires std::invocable<F, E &&> &&
                 detail::IsExpected<std::remove_cvref_t<std::invoke_result_t<F, E &&>>>::value &&
                 std::same_as<
                     typename std::remove_cvref_t<std::invoke_result_t<F, E &&>>::value_type, void>
    [[nodiscard]] constexpr auto
    OrElse(F &&f) && -> std::remove_cvref_t<std::invoke_result_t<F, E &&>>
    {
        using R = std::remove_cvref_t<std::invoke_result_t<F, E &&>>;
        if (inner_)
            return R(std::move(inner_));
        return std::invoke(std::forward<F>(f), std::move(inner_.error()));
    }

    template <typename F>
        requires std::invocable<F, E &>
    [[nodiscard]] constexpr auto
    TransformError(F &&f) & -> Expected<void, std::remove_cvref_t<std::invoke_result_t<F, E &>>>
    {
        using E2 = std::remove_cvref_t<std::invoke_result_t<F, E &>>;
        if (inner_)
            return Expected<void, E2>{};
        return Unex(std::invoke(std::forward<F>(f), inner_.error()));
    }

    template <typename F>
        requires std::invocable<F, const E &>
    [[nodiscard]] constexpr auto TransformError(
        F &&f
    ) const & -> Expected<void, std::remove_cvref_t<std::invoke_result_t<F, const E &>>>
    {
        using E2 = std::remove_cvref_t<std::invoke_result_t<F, const E &>>;
        if (inner_)
            return Expected<void, E2>{};
        return Unex(std::invoke(std::forward<F>(f), inner_.error()));
    }

    template <typename F>
        requires std::invocable<F, E &&>
    [[nodiscard]] constexpr auto
    TransformError(F &&f) && -> Expected<void, std::remove_cvref_t<std::invoke_result_t<F, E &&>>>
    {
        using E2 = std::remove_cvref_t<std::invoke_result_t<F, E &&>>;
        if (inner_)
            return Expected<void, E2>{};
        return Unex(std::invoke(std::forward<F>(f), std::move(inner_.error())));
    }

private:
    StdExpected inner_;
};

} // namespace ws
