#pragma once

#include "expected.hpp"
#include "integers.hpp"
#include "text.hpp"

#include <optional>
#include <type_traits>

#include <ada.h>
#include <ada/url_aggregator.h>

namespace ws {

struct [[nodiscard]] UrlError final {
    enum class Code {
        kInputTooLong,
        kFailedToParse,
    };
    Code code;
};

class [[nodiscard]] Url final {
public:
    enum class StripOptions {
        kNone = 0,
        kPort = 1 << 0,
        kQuery = 1 << 1,
        kHash = 1 << 2,
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

    [[nodiscard]] static std::optional<Url> FromText(const String &text);
    [[nodiscard]] static Expected<Url, UrlError>
    FromBoundedSizeText(const String &text, usize max_bytes);
    [[nodiscard]] static Url FromParsed(ada::url_aggregator url);

    [[nodiscard]] String Host() const;
    [[nodiscard]] String Hostname() const;
    [[nodiscard]] String Port() const;
    [[nodiscard]] String Pathname() const;
    [[nodiscard]] String Search() const;
    [[nodiscard]] String PathWithSearch() const;
    [[nodiscard]] String Href() const;
    [[nodiscard]] String Origin() const;
    [[nodiscard]] String Surt() const;

    [[nodiscard]] bool HasHostname() const;
    [[nodiscard]] bool HasPort() const;
    [[nodiscard]] bool HasNonDefaultPort() const;
    [[nodiscard]] bool HasSearch() const;
    [[nodiscard]] bool HasValidDomain() const;
    [[nodiscard]] ada::scheme::type SchemeType() const;
    [[nodiscard]] bool IsHttp() const;
    [[nodiscard]] bool IsHttps() const;
    [[nodiscard]] bool IsHttpOrHttps() const;
    [[nodiscard]] Url Without(StripOptions options) const;
    [[nodiscard]] Url WithProtocol(const String &protocol) const;
    [[nodiscard]] Url WithHostname(const String &hostname) const;
    [[nodiscard]] Url WithPort(const String &port) const;
    [[nodiscard]] Url WithPathname(const String &pathname) const;
    [[nodiscard]] Url WithSearch(const String &search) const;

    [[nodiscard]] ada::url_aggregator CopyParsed() const;

private:
    explicit Url(ada::url_aggregator ada_url);

    ada::url_aggregator ada_url_;
};

} // namespace ws
