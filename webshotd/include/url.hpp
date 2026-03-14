#pragma once

#include "text.hpp"

#include <optional>

#include <ada.h>
#include <ada/url_aggregator.h>

namespace v1 {

class [[nodiscard]] Url final {
public:
    [[nodiscard]] static std::optional<Url> fromText(const String &text);
    [[nodiscard]] static Url fromTextThrow(const String &text);
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

    [[nodiscard]] ada::url_aggregator copyParsed() const;

private:
    explicit Url(ada::url_aggregator url);

    ada::url_aggregator adaUrl;
};

} // namespace v1
