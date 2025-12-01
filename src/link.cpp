#include "link.hpp"
/**
 * @file
 * @brief URL normalization and validation using ada.
 *
 * Contains helpers to sanitize user input, enforce scheme/host rules, and
 * produce a stable scheme‑less key for storage and lookups.
 */
#include "text.hpp"

#include <cctype>
#include <string>
#include <string_view>

#include <arpa/inet.h>

#include <ada.h>
#include <ada/url_aggregator.h>

#include <absl/strings/ascii.h>

#include <fmt/format.h>

namespace {

/** ASCII letter check without locale side effects. */
static bool isAsciiAlpha(char c) noexcept
{
    const unsigned char u = static_cast<unsigned char>(c);
    return u < 0x80 && std::isalpha(u) != 0;
}
/** ASCII alnum check without locale side effects. */
static bool isAsciiAlnum(char c) noexcept
{
    const unsigned char u = static_cast<unsigned char>(c);
    return u < 0x80 && std::isalnum(u) != 0;
}

static bool isIpLiteralHostname(std::string_view hostname) noexcept
{
    if (hostname.empty())
        return false;
    // Bracketed IPv6 literal per RFC 3986
    if (hostname.front() == '[' && hostname.back() == ']') {
        const std::string inside(hostname.substr(1, hostname.size() - 2));
        in6_addr addr6{};
        return inet_pton(AF_INET6, inside.c_str(), &addr6) == 1;
    }
    // Plain IPv4 dotted-decimal
    in_addr addr4{};
    std::string hostStr(hostname);
    return inet_pton(AF_INET, hostStr.c_str(), &addr4) == 1;
}

/** RFC 3986 scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) */
static bool isValidScheme(std::string_view sv) noexcept
{
    if (sv.empty() || !isAsciiAlpha(sv.front()))
        return false;
    for (size_t i = 1; i < sv.size(); i++) {
        const char c = sv[i];
        if (!(isAsciiAlnum(c) || c == '+' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

std::string_view serializeHref(const ada::url_aggregator &url)
{
    auto href = url.get_href();
    return href.back() == '/' ? href.substr(0, href.size() - 1) : href;
}

} // namespace

namespace v1 {

Link fromTextImpl(const String &text, size_t queryPartLengthMax, bool stripPort, bool stripQuery)
{
    std::string in(text.view());
    absl::StripAsciiWhitespace(&in);
    if (in.rfind("//", 0) == 0)
        throw InvalidLinkException("missing scheme");
    const auto schemePos = in.find("://");
    if (schemePos == std::string::npos ||
        !isValidScheme(std::string_view(in).substr(0, schemePos))) {
        in = fmt::format("http://{}", in);
    } else {
        std::string scheme = in.substr(0, schemePos);
        if (!(scheme == "http" || scheme == "https"))
            throw InvalidLinkException("unsupported scheme");
    }
    auto url = ada::parse<ada::url_aggregator>(in);
    if (!url)
        throw InvalidLinkException("failed to parse");
    if (url->type != ada::scheme::type::HTTP && url->type != ada::scheme::type::HTTPS)
        throw InvalidLinkException("unsupported scheme");
    if (!url->has_hostname() || url->get_hostname().empty())
        throw InvalidLinkException("missing hostname");

    if (isIpLiteralHostname(url->get_hostname()))
        throw InvalidLinkException("ip address not allowed");

    if (!url->has_valid_domain())
        throw InvalidLinkException("invalid host");
    if (url->get_search().size() > queryPartLengthMax)
        throw InvalidLinkException("query too long");

    url->set_username("");
    url->set_password("");
    url->clear_hash();
    if (stripPort)
        url->clear_port();
    if (stripQuery)
        url->set_search("");

    if (auto hostname = url->get_hostname(); !hostname.empty() && hostname.back() == '.')
        url->set_hostname(std::string(begin(hostname), end(hostname) - 1));

    Link out;
    out.url = std::move(*url);
    return out;
}

Link Link::fromTextStripPort(const String &text, size_t queryPartLengthMax)
{
    return fromTextImpl(text, queryPartLengthMax, true, false);
}

Link Link::fromText(const String &text, size_t queryPartLengthMax)
{
    return fromTextImpl(text, queryPartLengthMax, false, false);
}

Link Link::fromTextStripPortQuery(const String &text, size_t queryPartLengthMax)
{
    return fromTextImpl(text, queryPartLengthMax, true, true);
}

String Link::host() const { return String::fromBytesThrow(url.get_hostname()); }

String Link::httpUrl() const
{
    auto copy = url;
    copy.set_protocol("http");
    return String::fromBytesThrow(serializeHref(copy));
}

String Link::httpsUrl() const
{
    auto copy = url;
    copy.set_protocol("https");
    return String::fromBytesThrow(serializeHref(copy));
}

String Link::normalized() const
{
    auto copy = url;
    copy.set_protocol("http");
    constexpr std::string_view kHttpPrefix = "http://";
    return String::fromBytesThrow(serializeHref(copy).substr(kHttpPrefix.size()));
}

} // namespace v1
