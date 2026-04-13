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

#include <format>
#include <string>
#include <string_view>

#include <absl/strings/ascii.h>
#include <absl/strings/strip.h>

namespace {

/** RFC 3986 scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) */
static bool isValidScheme(std::string_view sv) noexcept
{
    if (sv.empty() || !absl::ascii_isalpha(static_cast<unsigned char>(sv.front())))
        return false;
    for (const char c : sv.substr(1)) {
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
    return std::string{href};
}

} // namespace

namespace v1 {

namespace {

Expected<Link, LinkError>
fromTextImpl(const String &text, usize urlBytesMax, Link::FromTextOptions options)
{
    using enum Link::FromTextOptions;
    const bool stripPort = Link::hasOption(options, kStripPort);
    const bool stripQuery = Link::hasOption(options, kStripQuery);

    std::string in(text.view());
    absl::StripAsciiWhitespace(&in);
    if (in.rfind("//", 0) == 0)
        return std::unexpected(LinkError{.code = LinkError::Code::kMissingScheme});
    const auto schemePos = in.find("://");
    if (schemePos == std::string::npos ||
        !isValidScheme(std::string_view(in).substr(0, schemePos))) {
        in = std::format("http://{}", in);
    } else {
        std::string scheme = in.substr(0, schemePos);
        if (!(scheme == "http" || scheme == "https"))
            return std::unexpected(LinkError{.code = LinkError::Code::kUnsupportedScheme});
    }
    if (usz(in) > urlBytesMax)
        return std::unexpected(LinkError{.code = LinkError::Code::kUrlTooLong});
    auto url = ada::parse<ada::url_aggregator>(in);
    if (!url)
        return std::unexpected(LinkError{.code = LinkError::Code::kFailedToParse});
    if (url->type != ada::scheme::type::HTTP && url->type != ada::scheme::type::HTTPS)
        return std::unexpected(LinkError{.code = LinkError::Code::kUnsupportedScheme});
    if (!url->has_hostname() || url->get_hostname().empty())
        return std::unexpected(LinkError{.code = LinkError::Code::kMissingHostname});

    if (isIpLiteralHostname(url->get_hostname()))
        return std::unexpected(LinkError{.code = LinkError::Code::kIpAddressNotAllowed});

    if (!url->has_valid_domain())
        return std::unexpected(LinkError{.code = LinkError::Code::kInvalidHost});

    url->set_username("");
    url->set_password("");
    url->clear_hash();
    if (stripPort)
        url->clear_port();
    if (stripQuery)
        url->set_search("");

    if (auto hostname = url->get_hostname(); !hostname.empty() && hostname.back() == '.')
        url->set_hostname(std::string(begin(hostname), end(hostname) - 1));

    return Link{Url::fromParsed(std::move(url.value()))};
}

} // namespace

Expected<Link, LinkError>
Link::fromText(const String &text, usize urlBytesMax, FromTextOptions options)
{
    return fromTextImpl(text, urlBytesMax, options);
}

String Link::host() const { return url.hostname(); }

String Link::httpUrl() const
{
    auto copy = url.copyParsed();
    copy.set_protocol("http");
    return String::fromBytes(serializeHref(copy)).expect();
}

String Link::httpsUrl() const
{
    auto copy = url.copyParsed();
    copy.set_protocol("https");
    return String::fromBytes(serializeHref(copy)).expect();
}

String Link::normalized() const
{
    auto copy = url.copyParsed();
    copy.set_protocol("http");
    constexpr std::string_view kHttpPrefix = "http://";
    return String::fromBytes(serializeHref(copy).substr(kHttpPrefix.size())).expect();
}

} // namespace v1
