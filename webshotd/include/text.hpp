#pragma once

/**
 * @file
 * @brief Normalized UTF-8 text helpers.
 */

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <uni_algo/conv.h>
#include <uni_algo/norm.h>

#include "expected.hpp"

namespace text {

using v1::Expected;

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

    [[nodiscard]] static constexpr Expected<String, TextError> fromBytes(std::string_view bytes)
    {
        if (!una::is_valid_utf8(bytes)) {
            return std::unexpected(
                TextError{
                    .code = TextError::Code::kInvalidUtf8,
                }
            );
        }
        String result;
        result.data = una::norm::to_nfc_utf8(bytes);
        return result;
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept
    {
        return {data.data(), data.size()};
    }

    [[nodiscard]] constexpr bool empty() const noexcept { return data.empty(); }

    [[nodiscard]] constexpr size_t sizeBytes() const noexcept { return data.size(); }

    [[nodiscard]] constexpr bool startsWith(const String &prefix) const noexcept
    {
        if (prefix.data.size() > data.size())
            return false;
        const auto dataPrefix = std::string_view{data}.substr(0, prefix.data.size());
        return std::ranges::equal(prefix.data, dataPrefix);
    }

    [[nodiscard]] constexpr bool startsWith(std::string_view prefix) const noexcept
    {
        return data.starts_with(prefix);
    }

    [[nodiscard]] constexpr bool startsWith(char prefix) const noexcept
    {
        return data.starts_with(prefix);
    }

    [[nodiscard]] constexpr bool endsWith(std::string_view suffix) const noexcept
    {
        return data.ends_with(suffix);
    }

    [[nodiscard]] constexpr bool endsWith(char suffix) const noexcept
    {
        return data.ends_with(suffix);
    }

    [[nodiscard]] constexpr bool endsWith(const String &suffix) const noexcept
    {
        return data.ends_with(std::string_view{suffix.data});
    }

    constexpr String &operator+=(const String &rhs)
    {
        if (rhs.data.empty())
            return *this;
        std::string combined;
        combined.reserve(data.size() + rhs.data.size());
        combined.assign(data);
        combined += rhs.data;
        data = una::norm::to_nfc_utf8({combined.data(), combined.size()});
        return *this;
    }

    [[nodiscard]] constexpr String reversed() const
    {
        auto utf32 = una::utf8to32u(data);
        std::ranges::reverse(utf32);

        const auto reversedUtf8 = una::utf32to8(utf32);

        String result;
        result.data = una::norm::to_nfc_utf8({reversedUtf8.data(), reversedUtf8.size()});
        return result;
    }

    [[nodiscard]] friend constexpr bool operator==(const String &lhs, const String &rhs) noexcept
    {
        return lhs.data == rhs.data;
    }

    [[nodiscard]] friend constexpr bool operator<(const String &lhs, const String &rhs) noexcept
    {
        return lhs.data < rhs.data;
    }

private:
    std::string data;
};

[[nodiscard]] constexpr String operator+(String lhs, const String &rhs)
{
    lhs += rhs;
    return lhs;
}

template <typename... Ts> String format(std::format_string<Ts...> formatStr, Ts &&...args)
{
    return String::fromBytes(std::format(formatStr, std::forward<Ts>(args)...)).expect();
}

namespace literals {
[[nodiscard]] constexpr String operator""_t(const char *bytes, size_t n)
{
    return String::fromBytes(std::string_view{bytes, n}).expect();
}
} // namespace literals

} // namespace text

using text::String;

template <> struct std::formatter<text::String, char> : std::formatter<std::string_view, char> {
    auto format(const text::String &text, std::format_context &ctx) const
    {
        return std::formatter<std::string_view, char>::format(text.view(), ctx);
    }
};

template <> struct std::hash<String> {
    size_t operator()(const String &text) const noexcept
    {
        return std::hash<std::string_view>{}(text.view());
    }
};
