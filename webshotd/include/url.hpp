#pragma once

#include "text.hpp"

#include <optional>
#include <type_traits>

#include <ada.h>
#include <ada/url_aggregator.h>

namespace v1 {

class [[nodiscard]] Url final {
public:
    enum class StripOptions {
        kNone = 0,
        kStripPort = 1 << 0,
        kStripQuery = 1 << 1,
    };

    friend constexpr StripOptions operator|(StripOptions lhs, StripOptions rhs) noexcept
    {
        using U = std::underlying_type_t<StripOptions>;
        return static_cast<StripOptions>(static_cast<U>(lhs) | static_cast<U>(rhs));
    }

    friend constexpr StripOptions operator&(StripOptions lhs, StripOptions rhs) noexcept
    {
        using U = std::underlying_type_t<StripOptions>;
        return static_cast<StripOptions>(static_cast<U>(lhs) & static_cast<U>(rhs));
    }

    [[nodiscard]] static std::optional<Url> fromText(const String &text);
    [[nodiscard]] static Url fromParsed(ada::url_aggregator url);

    [[nodiscard]] String host() const;
    [[nodiscard]] String hostname() const;
    [[nodiscard]] String port() const;
    [[nodiscard]] String pathname() const;
    [[nodiscard]] String search() const;
    [[nodiscard]] String pathWithSearch() const;
    [[nodiscard]] String href() const;

    [[nodiscard]] bool hasHostname() const;
    [[nodiscard]] bool hasPort() const;
    [[nodiscard]] bool hasSearch() const;
    [[nodiscard]] bool hasValidDomain() const;
    [[nodiscard]] ada::scheme::type schemeType() const;
    [[nodiscard]] bool isHttp() const;
    [[nodiscard]] bool isHttps() const;
    [[nodiscard]] Url stripped(StripOptions options) const;

    [[nodiscard]] ada::url_aggregator copyParsed() const;

private:
    explicit Url(ada::url_aggregator adaUrl);

    ada::url_aggregator adaUrl;
};

} // namespace v1
