#include "link.hpp"
/**
 * @file
 * @brief URL normalization and validation using ada.
 *
 * Contains helpers to sanitize user input, enforce scheme/host rules, and
 * produce a stable scheme-less key for storage and lookups.
 */
#include "ip.hpp"
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

Expected<Link, LinkError> fromTextImpl(const String &text, usize urlBytesMax)
{
    using enum LinkError::Code;

    std::string in(text.view());
    absl::StripAsciiWhitespace(&in);
    if (in.starts_with("//"))
        return Unex(LinkError{.code = kMissingScheme});
    const auto schemePos = in.find("://");
    if (schemePos == std::string::npos ||
        !isValidScheme(std::string_view(in).substr(0, schemePos))) {
        in = std::format("http://{}", in);
    } else {
        std::string scheme = in.substr(0, schemePos);
        if (!(scheme == "http" || scheme == "https"))
            return Unex(LinkError{.code = kUnsupportedScheme});
    }
    if (usz(in) > urlBytesMax)
        return Unex(LinkError{.code = kUrlTooLong});
    auto url = ada::parse<ada::url_aggregator>(in);
    if (!url)
        return Unex(LinkError{.code = kFailedToParse});
    auto parsedUrl = Url::fromParsed(std::move(*url));
    if (!parsedUrl.isHttpOrHttps())
        return Unex(LinkError{.code = kUnsupportedScheme});
    if (!parsedUrl.hasHostname())
        return Unex(LinkError{.code = kMissingHostname});

    if (isIpLiteralHostname(parsedUrl.hostname()))
        return Unex(LinkError{.code = kIpAddressNotAllowed});

    if (!parsedUrl.hasValidDomain())
        return Unex(LinkError{.code = kInvalidHost});

    auto normalized = parsedUrl.copyParsed();
    normalized.set_username("");
    normalized.set_password("");
    normalized.clear_hash();
    normalized.clear_port();

    if (auto hostname = normalized.get_hostname(); !hostname.empty() && hostname.back() == '.')
        normalized.set_hostname(std::string(begin(hostname), end(hostname) - 1));

    return Link{Url::fromParsed(std::move(normalized))};
}

} // namespace

Expected<Link, LinkError> Link::fromText(const String &text, usize urlBytesMax)
{
    return fromTextImpl(text, urlBytesMax);
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
