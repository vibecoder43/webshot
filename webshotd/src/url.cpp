#include "url.hpp"

#include <string>
#include <utility>

namespace v1 {

namespace {

constexpr bool hasStripOption(Url::StripOptions options, Url::StripOptions flag) noexcept
{
    using U = std::underlying_type_t<Url::StripOptions>;
    return static_cast<U>(options & flag) != 0;
}

} // namespace

Url::Url(ada::url_aggregator adaUrl) : adaUrl(std::move(adaUrl)) {}

std::optional<Url> Url::fromText(const String &text)
{
    auto parsed = ada::parse<ada::url_aggregator>(text.view());
    if (!parsed)
        return {};
    return Url(std::move(*parsed));
}

Url Url::fromParsed(ada::url_aggregator url) { return Url(std::move(url)); }

String Url::host() const { return String::fromBytes(adaUrl.get_host()).expect(); }

String Url::hostname() const { return String::fromBytes(adaUrl.get_hostname()).expect(); }

String Url::port() const { return String::fromBytes(adaUrl.get_port()).expect(); }

String Url::pathname() const { return String::fromBytes(adaUrl.get_pathname()).expect(); }

String Url::search() const { return String::fromBytes(adaUrl.get_search()).expect(); }

String Url::pathWithSearch() const
{
    auto path = std::string(adaUrl.get_pathname());
    if (path.empty())
        path = "/";
    path += std::string(adaUrl.get_search());
    return String::fromBytes(path).expect();
}

String Url::href() const { return String::fromBytes(adaUrl.get_href()).expect(); }

bool Url::hasHostname() const { return adaUrl.has_hostname() && !adaUrl.get_hostname().empty(); }

bool Url::hasPort() const { return adaUrl.has_port(); }

bool Url::hasSearch() const { return adaUrl.has_search(); }

bool Url::hasValidDomain() const { return adaUrl.has_valid_domain(); }

ada::scheme::type Url::schemeType() const { return adaUrl.type; }

bool Url::isHttp() const { return adaUrl.type == ada::scheme::type::HTTP; }

bool Url::isHttps() const { return adaUrl.type == ada::scheme::type::HTTPS; }

Url Url::stripped(StripOptions options) const
{
    auto parsed = copyParsed();
    if (hasStripOption(options, StripOptions::kStripPort))
        parsed.clear_port();
    if (hasStripOption(options, StripOptions::kStripQuery))
        parsed.clear_search();
    return Url::fromParsed(std::move(parsed));
}

ada::url_aggregator Url::copyParsed() const { return adaUrl; }

} // namespace v1
