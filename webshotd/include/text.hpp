#pragma once

/**
 * @file
 * @brief Normalized UTF-8 text helpers.
 */

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <uni_algo/conv.h>
#include <uni_algo/norm.h>

#include <fmt/format.h>

namespace text {

struct InvalidTextException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

class [[nodiscard]] String {
public:
    constexpr String() = default;
    constexpr String(const String &) = default;
    constexpr String(String &&) noexcept = default;
    constexpr String &operator=(const String &) = default;
    constexpr String &operator=(String &&) noexcept = default;
    ~String() = default;

    [[nodiscard]] static constexpr std::optional<String> fromBytes(std::string_view bytes)
    {
        if (!una::is_valid_utf8(bytes))
            return {};
        String result;
        result.data = una::norm::to_nfc_utf8(bytes);
        return result;
    }

    [[nodiscard]] static constexpr String fromBytesThrow(std::string_view bytes)
    {
        auto ret = fromBytes(bytes);
        if (!ret)
            throw InvalidTextException("not valid UTF-8");
        return ret.value();
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
        return std::equal(std::begin(prefix.data), std::end(prefix.data), std::begin(data));
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
        std::reverse(std::begin(utf32), std::end(utf32));

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

template <typename... Ts> String format(fmt::format_string<Ts...> fmt, Ts &&...args)
{
    return String::fromBytesThrow(fmt::format(fmt, std::forward<Ts>(args)...));
}

namespace literals {
[[nodiscard]] constexpr String operator""_t(const char *bytes, size_t n)
{
    return String::fromBytesThrow(std::string_view{bytes, n});
}
} // namespace literals

} // namespace text

using text::String;

template <> struct fmt::formatter<String> : fmt::formatter<std::string_view> {
    fmt::format_context::iterator format(const String &text, fmt::format_context &ctx) const
    {
        return fmt::formatter<std::string_view>::format(text.view(), ctx);
    }
};

template <> struct std::hash<String> {
    size_t operator()(const String &text) const noexcept
    {
        return std::hash<std::string_view>{}(text.view());
    }
};
