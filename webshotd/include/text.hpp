#pragma once

/**
 * @file
 * @brief Normalized UTF-8 text helpers.
 */

#include <algorithm>
#include <concepts>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <uni_algo/conv.h>
#include <uni_algo/norm.h>

#include <fmt/format.h>

#include "expected.hpp"
#include "try.hpp"

namespace text {

using ws::Expected;
using ws::Unex;

struct [[nodiscard]] TextError final {
    enum class Code {
        kInvalidUtf8,
    };
    Code code;
};

class [[nodiscard]] String {
public:
    constexpr String() = default;
    constexpr String(const String &) = default;
    constexpr String(String &&) noexcept = default;
    constexpr String &operator=(const String &) = default;
    constexpr String &operator=(String &&) noexcept = default;
    ~String() = default;

    [[nodiscard]] static constexpr Expected<String, TextError> FromBytes(std::string_view bytes)
    {
        if (!una::is_valid_utf8(bytes)) {
            return Unex(
                TextError{
                    .code = TextError::Code::kInvalidUtf8,
                }
            );
        }
        String result;
        result.data_ = una::norm::to_nfc_utf8(bytes);
        return result;
    }

    [[nodiscard]] constexpr std::string_view View() const noexcept
    {
        return {data_.data(), data_.size()};
    }

    [[nodiscard]] std::string ToBytes() const { return std::string{View()}; }

    [[nodiscard]] constexpr bool Empty() const noexcept { return data_.empty(); }

    [[nodiscard]] constexpr size_t SizeBytes() const noexcept { return data_.size(); }

    [[nodiscard]] constexpr bool StartsWith(const String &prefix) const noexcept
    {
        if (prefix.data_.size() > data_.size())
            return false;
        const std::string_view data_prefix{std::string_view{data_}.substr(0, prefix.data_.size())};
        return std::ranges::equal(prefix.data_, data_prefix);
    }

    [[nodiscard]] constexpr bool StartsWith(std::string_view prefix) const noexcept
    {
        return data_.starts_with(prefix);
    }

    [[nodiscard]] constexpr bool StartsWith(char prefix) const noexcept
    {
        return data_.starts_with(prefix);
    }

    [[nodiscard]] constexpr bool EndsWith(std::string_view suffix) const noexcept
    {
        return data_.ends_with(suffix);
    }

    [[nodiscard]] constexpr bool EndsWith(char suffix) const noexcept
    {
        return data_.ends_with(suffix);
    }

    [[nodiscard]] constexpr bool EndsWith(const String &suffix) const noexcept
    {
        return data_.ends_with(std::string_view{suffix.data_});
    }

    constexpr String &operator+=(const String &rhs)
    {
        if (rhs.data_.empty())
            return *this;
        std::string combined;
        combined.reserve(data_.size() + rhs.data_.size());
        combined.assign(data_);
        combined += rhs.data_;
        data_ = una::norm::to_nfc_utf8({combined.data(), combined.size()});
        return *this;
    }

    [[nodiscard]] constexpr String Reversed() const
    {
        auto utf32 = una::utf8to32u(data_);
        std::ranges::reverse(utf32);

        auto reversed_utf8 = una::utf32to8(utf32);

        String result;
        result.data_ = una::norm::to_nfc_utf8({reversed_utf8.data(), reversed_utf8.size()});
        return result;
    }

    [[nodiscard]] friend constexpr bool operator==(const String &lhs, const String &rhs) noexcept
    {
        return lhs.data_ == rhs.data_;
    }

    [[nodiscard]] friend constexpr bool operator<(const String &lhs, const String &rhs) noexcept
    {
        return lhs.data_ < rhs.data_;
    }

private:
    std::string data_;
};

namespace detail {

template <typename T> [[nodiscard]] constexpr std::string_view ByteView(const T &value) noexcept
{
    return {value.data(), value.size()};
}

template <typename Range, typename F>
using CollectExpectedResult =
    std::remove_cvref_t<std::invoke_result_t<F, std::ranges::range_reference_t<const Range>>>;

template <std::ranges::input_range Range, typename F>
    requires std::invocable<F, std::ranges::range_reference_t<const Range>>
[[nodiscard]] auto CollectExpected(const Range &range, F &&f) -> Expected<
    std::vector<typename CollectExpectedResult<Range, F>::value_type>,
    typename CollectExpectedResult<Range, F>::error_type>
{
    using ExpectedResult = CollectExpectedResult<Range, F>;
    using Value = typename ExpectedResult::value_type;

    std::vector<Value> out;
    if constexpr (std::ranges::sized_range<const Range>)
        out.reserve(std::ranges::size(range));

    for (const auto &item : range) {
        out.push_back(TRY(std::invoke(f, item)));
    }
    return {std::move(out)};
}

} // namespace detail

template <std::ranges::input_range Range>
[[nodiscard]] Expected<std::vector<String>, TextError> StringVector(const Range &bytes)
{
    return detail::CollectExpected(bytes, [](const auto &value) {
        return String::FromBytes(detail::ByteView(value));
    });
}

template <std::ranges::input_range Range>
[[nodiscard]] Expected<std::vector<std::pair<String, String>>, TextError>
StringPairs(const Range &pairs)
{
    using Pair = std::pair<String, String>;
    using Result = Expected<Pair, TextError>;

    return detail::CollectExpected(pairs, [](const auto &pair) -> Result {
        auto first = TRY(String::FromBytes(detail::ByteView(pair.first)));
        auto second = TRY(String::FromBytes(detail::ByteView(pair.second)));
        return Pair{std::move(first), std::move(second)};
    });
}

template <typename T>
[[nodiscard]] Expected<std::optional<String>, TextError>
OptionalString(const std::optional<T> &bytes)
{
    if (!bytes)
        return {};

    return String::FromBytes(detail::ByteView(*bytes)).Transform([](String text) {
        return std::optional<String>{std::move(text)};
    });
}

template <std::ranges::input_range Range>
[[nodiscard]] std::vector<std::string> ToBytesVector(const Range &texts)
{
    std::vector<std::string> out;
    if constexpr (std::ranges::sized_range<const Range>)
        out.reserve(std::ranges::size(texts));

    for (const auto &text : texts)
        out.push_back(text.ToBytes());
    return out;
}

template <std::ranges::input_range Range>
[[nodiscard]] std::vector<std::pair<std::string, std::string>> ToBytesPairs(const Range &pairs)
{
    std::vector<std::pair<std::string, std::string>> out;
    if constexpr (std::ranges::sized_range<const Range>)
        out.reserve(std::ranges::size(pairs));

    for (const auto &[first, second] : pairs)
        out.emplace_back(first.ToBytes(), second.ToBytes());
    return out;
}

[[nodiscard]] constexpr String operator+(String lhs, const String &rhs)
{
    lhs += rhs;
    return lhs;
}

template <typename... Ts> String Format(std::format_string<Ts...> format_str, Ts &&...args)
{
    return *String::FromBytes(std::format(format_str, std::forward<Ts>(args)...));
}

namespace literals {
[[nodiscard]] constexpr String operator""_t(const char *bytes, size_t n)
{
    return *String::FromBytes(std::string_view{bytes, n});
}
} // namespace literals

} // namespace text

using text::String;

template <> struct std::formatter<text::String, char> : std::formatter<std::string_view, char> {
    auto format(const text::String &text, std::format_context &ctx) const
    {
        return std::formatter<std::string_view, char>::format(text.View(), ctx);
    }
};

// userver is still using fmtlib
template <> struct fmt::formatter<text::String, char> : fmt::formatter<std::string_view, char> {
    auto format(const text::String &text, fmt::format_context &ctx) const
    {
        return fmt::formatter<std::string_view, char>::format(text.View(), ctx);
    }
};

template <> struct std::hash<String> {
    size_t operator()(const String &text) const noexcept
    {
        return std::hash<std::string_view>{}(text.View());
    }
};
