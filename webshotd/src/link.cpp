#include "link.hpp"
/**
 * @file
 * @brief URL normalization and validation using ada.
 *
 * Contains helpers to sanitize user input, enforce scheme/host rules, and
 * produce a stable scheme-less key for storage and lookups.
 */
#include "character_type.hpp"
#include "ip.hpp"
#include "text.hpp"

#include <format>
#include <string>
#include <string_view>

#include <absl/strings/strip.h>

namespace {

/** RFC 3986 scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) */
static bool IsValidScheme(std::string_view sv) noexcept
{
    if (sv.empty() || !ws::ctype::IsAsciiAlpha(sv.front()))
        return false;
    for (const char c : sv.substr(1)) {
        if (!(ws::ctype::IsAsciiAlnum(c) || c == '+' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

std::string SerializeHref(const ada::url_aggregator &url)
{
    auto href = std::string_view(url.get_href());
    absl::ConsumeSuffix(&href, "/");
    return std::string{href};
}

} // namespace

namespace ws {

namespace {

Expected<Url, LinkError> NormalizeUrl(Url url, usize url_bytes_max)
{
    using enum LinkError::Code;

    if (!url.IsHttpOrHttps())
        return Unex{LinkError{.code = kUnsupportedScheme}};
    if (!url.HasHostname())
        return Unex{LinkError{.code = kMissingHostname}};

    if (IsIpLiteralHostname(url.Hostname()))
        return Unex{LinkError{.code = kIpAddressNotAllowed}};

    if (!url.HasValidDomain())
        return Unex{LinkError{.code = kInvalidHost}};

    auto normalized = url.CopyParsed();
    normalized.set_username("");
    normalized.set_password("");
    normalized.clear_hash();
    normalized.clear_port();

    if (auto hostname = normalized.get_hostname(); !hostname.empty() && hostname.back() == '.')
        normalized.set_hostname(std::string(begin(hostname), end(hostname) - 1));

    auto normalized_href = SerializeHref(normalized);
    if (unsize(normalized_href) > url_bytes_max)
        return Unex{LinkError{.code = kNormalizedHrefTooLong}};

    return Url::FromParsed(std::move(normalized));
}

} // namespace

Link::Link(Url url) : url_(std::move(url)) {}

Expected<Link, LinkError> Link::FromText(const String &text, usize url_bytes_max)
{
    using enum LinkError::Code;

    std::string in{text.View()};
    absl::StripAsciiWhitespace(&in);
    if (in.starts_with("//"))
        return Unex{LinkError{.code = kMissingScheme}};
    auto scheme_pos = in.find("://");
    if (scheme_pos == std::string::npos ||
        !IsValidScheme(std::string_view{in}.substr(0, scheme_pos))) {
        in = std::format("http://{}", in);
    } else {
        auto scheme = in.substr(0, scheme_pos);
        if (!(scheme == "http" || scheme == "https"))
            return Unex{LinkError{.code = kUnsupportedScheme}};
    }
    auto parsed_or = Url::FromBoundedSizeText(*String::FromBytes(in), url_bytes_max);
    if (!parsed_or) {
        switch (parsed_or.Error().code) {
        case UrlError::Code::kInputTooLong:
            return Unex{LinkError{.code = kInputTooLong}};
        case UrlError::Code::kFailedToParse:
            return Unex{LinkError{.code = kFailedToParse}};
        }
    }
    auto normalized = TRY(NormalizeUrl(*parsed_or, url_bytes_max));
    return Link{std::move(normalized)};
}

Expected<Link, LinkError> Link::FromUrl(const Url &url, usize url_bytes_max)
{
    auto normalized = TRY(NormalizeUrl(url, url_bytes_max));
    return Link{std::move(normalized)};
}

String Link::Hostname() const { return url_.Hostname(); }

String Link::Pathname() const { return url_.Pathname(); }

String Link::HttpUrl() const
{
    auto copy = url_.CopyParsed();
    copy.set_protocol("http");
    return *String::FromBytes(SerializeHref(copy));
}

String Link::HttpsUrl() const
{
    auto copy = url_.CopyParsed();
    copy.set_protocol("https");
    return *String::FromBytes(SerializeHref(copy));
}

String Link::ToKey() const
{
    auto copy = url_.CopyParsed();
    copy.set_protocol("http");
    constexpr std::string_view http_prefix = "http://";
    return *String::FromBytes(SerializeHref(copy).substr(http_prefix.size()));
}

} // namespace ws
