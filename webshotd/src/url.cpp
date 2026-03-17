#include "url.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace v1 {

Url::Url(ada::url_aggregator url) : adaUrl(std::move(url)) {}

std::optional<Url> Url::fromText(const String &text)
{
    auto parsed = ada::parse<ada::url_aggregator>(text.view());
    if (!parsed)
        return {};
    return Url(std::move(parsed.value()));
}

Url Url::fromTextThrow(const String &text)
{
    const auto maybeUrl = fromText(text);
    if (!maybeUrl)
        throw std::runtime_error("invalid url");
    return maybeUrl.value();
}

Url Url::fromParsed(ada::url_aggregator url) { return Url(std::move(url)); }

String Url::host() const { return String::fromBytesThrow(adaUrl.get_host()); }

String Url::hostname() const { return String::fromBytesThrow(adaUrl.get_hostname()); }

String Url::port() const { return String::fromBytesThrow(adaUrl.get_port()); }

String Url::pathname() const { return String::fromBytesThrow(adaUrl.get_pathname()); }

String Url::search() const { return String::fromBytesThrow(adaUrl.get_search()); }

String Url::pathWithSearch() const
{
    auto path = std::string(adaUrl.get_pathname());
    if (path.empty())
        path = "/";
    path += std::string(adaUrl.get_search());
    return String::fromBytesThrow(path);
}

String Url::href() const { return String::fromBytesThrow(adaUrl.get_href()); }

bool Url::hasHostname() const { return adaUrl.has_hostname() && !adaUrl.get_hostname().empty(); }

bool Url::hasPort() const { return adaUrl.has_port(); }

bool Url::hasSearch() const { return adaUrl.has_search(); }

bool Url::hasValidDomain() const { return adaUrl.has_valid_domain(); }

ada::scheme::type Url::schemeType() const { return adaUrl.type; }

bool Url::isHttp() const { return adaUrl.type == ada::scheme::type::HTTP; }

bool Url::isHttps() const { return adaUrl.type == ada::scheme::type::HTTPS; }

ada::url_aggregator Url::copyParsed() const { return adaUrl; }

} // namespace v1
