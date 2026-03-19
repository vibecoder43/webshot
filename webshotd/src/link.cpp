#include "link.hpp"
/**
 * @file
 * @brief URL normalization and validation using ada.
 *
 * Contains helpers to sanitize user input, enforce scheme/host rules, and
 * produce a stable scheme-less key for storage and lookups.
 */
#include "ip_utils.hpp"
#include "text.hpp"

#include <string>
#include <string_view>

#include <absl/strings/ascii.h>
#include <absl/strings/strip.h>

#include <fmt/format.h>

namespace {

/** RFC 3986 scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) */
static bool isValidScheme(std::string_view sv) noexcept
{
    if (sv.empty() || !absl::ascii_isalpha(static_cast<unsigned char>(sv.front())))
        return false;
    for (size_t i = 1; i < sv.size(); i++) {
        const char c = sv[i];
        if (!(absl::ascii_isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' ||
              c == '.'))
            return false;
    }
    return true;
}

std::string serializeHref(const ada::url_aggregator &url)
{
    auto href = std::string_view(url.get_href());
    absl::ConsumeSuffix(&href, "/");
    return std::string(href);
}

} // namespace

namespace v1 {

namespace {

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

    return {Url::fromParsed(std::move(url.value()))};
}

} // namespace

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

String Link::host() const { return url.hostname(); }

String Link::httpUrl() const
{
    auto copy = url.copyParsed();
    copy.set_protocol("http");
    return String::fromBytesThrow(serializeHref(copy));
}

String Link::httpsUrl() const
{
    auto copy = url.copyParsed();
    copy.set_protocol("https");
    return String::fromBytesThrow(serializeHref(copy));
}

String Link::normalized() const
{
    auto copy = url.copyParsed();
    copy.set_protocol("http");
    constexpr std::string_view kHttpPrefix = "http://";
    return String::fromBytesThrow(serializeHref(copy).substr(kHttpPrefix.size()));
}

} // namespace v1
