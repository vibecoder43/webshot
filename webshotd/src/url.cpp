#include "url.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "userver_namespaces.hpp"

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

String Url::origin() const
{
    invariant(isHttp() || isHttps(), "origin requires http or https url");
    return text::format("{}://{}", isHttps() ? "https" : "http", host());
}

String Url::surt() const
{
    invariant(isHttp() || isHttps(), "surt requires http or https url");

    std::string hostText{hostname().view()};
    std::string portText{port().view()};
    while (!hostText.empty() && hostText.back() == '.')
        hostText.pop_back();

    std::vector<std::string> labels;
    for (size_t offset = 0; offset <= hostText.size();) {
        const auto next = hostText.find('.', offset);
        if (next == std::string::npos) {
            labels.emplace_back(hostText.substr(offset));
            break;
        }
        labels.emplace_back(hostText.substr(offset, next - offset));
        offset = next + 1;
    }
    std::ranges::reverse(labels);

    std::string surtHost;
    for (size_t index = 0; index < labels.size(); index++) {
        if (index != 0)
            surtHost.push_back(',');
        surtHost += labels[index];
    }

    if (hasNonDefaultPort())
        surtHost += ":" + portText;

    return String::fromBytes(surtHost + ")" + std::to_string(pathWithSearch())).expect();
}

bool Url::hasHostname() const { return adaUrl.has_hostname() && !adaUrl.get_hostname().empty(); }

bool Url::hasPort() const { return adaUrl.has_port(); }

bool Url::hasNonDefaultPort() const
{
    if (!hasPort())
        return false;

    const auto defaultPort = ada::scheme::get_special_port(schemeType());
    if (defaultPort == 0)
        return true;

    return port() != String::fromBytes(std::to_string(defaultPort)).expect();
}

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

Url Url::withProtocol(const String &protocol) const
{
    auto parsed = copyParsed();
    invariant(parsed.set_protocol(std::to_string(protocol)), "invalid protocol");
    return Url::fromParsed(std::move(parsed));
}

Url Url::withHostname(const String &hostname) const
{
    auto parsed = copyParsed();
    invariant(parsed.set_hostname(std::to_string(hostname)), "invalid hostname");
    return Url::fromParsed(std::move(parsed));
}

Url Url::withPort(const String &portValue) const
{
    auto parsed = copyParsed();
    invariant(parsed.set_port(std::to_string(portValue)), "invalid port");
    return Url::fromParsed(std::move(parsed));
}

Url Url::withPathname(const String &pathnameValue) const
{
    auto parsed = copyParsed();
    invariant(parsed.set_pathname(pathnameValue.view()), "invalid pathname");
    return Url::fromParsed(std::move(parsed));
}

Url Url::withSearch(const String &searchValue) const
{
    auto parsed = copyParsed();
    parsed.set_search(searchValue.view());
    return Url::fromParsed(std::move(parsed));
}

Url Url::withoutSearch() const
{
    auto parsed = copyParsed();
    parsed.clear_search();
    return Url::fromParsed(std::move(parsed));
}

Url Url::withoutHash() const
{
    auto parsed = copyParsed();
    parsed.clear_hash();
    return Url::fromParsed(std::move(parsed));
}

ada::url_aggregator Url::copyParsed() const { return adaUrl; }

} // namespace v1
