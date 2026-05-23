#include "url.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "invariant.hpp"

namespace ws {

using namespace text::literals;

namespace {

constexpr bool HasStripOption(Url::StripOptions options, Url::StripOptions flag) noexcept
{
    using U = std::underlying_type_t<Url::StripOptions>;
    return static_cast<U>(options & flag) != 0;
}

} // namespace

Url::Url(ada::url_aggregator ada_url) : ada_url_(std::move(ada_url)) {}

std::optional<Url> Url::FromText(const String &text)
{
    auto parsed = ada::parse<ada::url_aggregator>(text.View());
    if (!parsed)
        return {};
    return Url(std::move(*parsed));
}

Url Url::FromParsed(ada::url_aggregator url) { return Url(std::move(url)); }

String Url::Host() const { return *String::FromBytes(ada_url_.get_host()); }

String Url::Hostname() const { return *String::FromBytes(ada_url_.get_hostname()); }

String Url::Port() const { return *String::FromBytes(ada_url_.get_port()); }

String Url::Pathname() const { return *String::FromBytes(ada_url_.get_pathname()); }

String Url::Search() const { return *String::FromBytes(ada_url_.get_search()); }

String Url::PathWithSearch() const
{
    auto path = std::string(ada_url_.get_pathname());
    if (path.empty())
        path = "/";
    path += std::string(ada_url_.get_search());
    return *String::FromBytes(path);
}

String Url::Href() const { return *String::FromBytes(ada_url_.get_href()); }

String Url::Origin() const
{
    Invariant(IsHttpOrHttps(), "origin requires http or https url"_t);
    return text::Format("{}://{}", IsHttps() ? "https" : "http", Host());
}

String Url::Surt() const
{
    Invariant(IsHttpOrHttps(), "surt requires http or https url"_t);

    std::string host_text{Hostname().View()};
    std::string port_text{Port().View()};
    while (!host_text.empty() && host_text.back() == '.')
        host_text.pop_back();

    std::vector<std::string> labels;
    for (size_t offset = 0; offset <= host_text.size();) {
        auto next = host_text.find('.', offset);
        if (next == std::string::npos) {
            labels.emplace_back(host_text.substr(offset));
            break;
        }
        labels.emplace_back(host_text.substr(offset, next - offset));
        offset = next + 1;
    }
    std::ranges::reverse(labels);

    std::string surt_host;
    for (size_t index = 0; index < labels.size(); index++) {
        if (index != 0)
            surt_host.push_back(',');
        surt_host += labels[index];
    }

    if (HasNonDefaultPort())
        surt_host += ":" + port_text;

    return *String::FromBytes(surt_host + ")" + PathWithSearch().ToBytes());
}

bool Url::HasHostname() const
{
    return ada_url_.has_hostname() && !ada_url_.get_hostname().empty();
}

bool Url::HasPort() const { return ada_url_.has_port(); }

bool Url::HasNonDefaultPort() const
{
    if (!HasPort())
        return false;

    auto default_port = ada::scheme::get_special_port(SchemeType());
    if (default_port == 0)
        return true;

    return Port() != *String::FromBytes(std::to_string(default_port));
}

bool Url::HasSearch() const { return ada_url_.has_search(); }

bool Url::HasValidDomain() const { return ada_url_.has_valid_domain(); }

ada::scheme::type Url::SchemeType() const { return ada_url_.type; }

bool Url::IsHttp() const { return ada_url_.type == ada::scheme::type::HTTP; }

bool Url::IsHttps() const { return ada_url_.type == ada::scheme::type::HTTPS; }

bool Url::IsHttpOrHttps() const { return IsHttp() || IsHttps(); }

Url Url::Stripped(StripOptions options) const
{
    auto parsed = CopyParsed();
    if (HasStripOption(options, StripOptions::kPort))
        parsed.clear_port();
    if (HasStripOption(options, StripOptions::kQuery))
        parsed.clear_search();
    if (HasStripOption(options, StripOptions::kHash))
        parsed.set_hash("");
    return Url::FromParsed(std::move(parsed));
}

Url Url::WithProtocol(const String &protocol) const
{
    auto parsed = CopyParsed();
    Invariant(parsed.set_protocol(protocol.ToBytes()), "invalid protocol"_t);
    return Url::FromParsed(std::move(parsed));
}

Url Url::WithHostname(const String &hostname) const
{
    auto parsed = CopyParsed();
    Invariant(parsed.set_hostname(hostname.ToBytes()), "invalid hostname"_t);
    return Url::FromParsed(std::move(parsed));
}

Url Url::WithPort(const String &port_value) const
{
    auto parsed = CopyParsed();
    Invariant(parsed.set_port(port_value.ToBytes()), "invalid port"_t);
    return Url::FromParsed(std::move(parsed));
}

Url Url::WithPathname(const String &pathname_value) const
{
    auto parsed = CopyParsed();
    Invariant(parsed.set_pathname(pathname_value.View()), "invalid pathname"_t);
    return Url::FromParsed(std::move(parsed));
}

Url Url::WithSearch(const String &search_value) const
{
    auto parsed = CopyParsed();
    parsed.set_search(search_value.View());
    return Url::FromParsed(std::move(parsed));
}

ada::url_aggregator Url::CopyParsed() const { return ada_url_; }

} // namespace ws
