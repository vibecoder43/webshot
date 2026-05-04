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
static bool IsValidScheme(std::string_view sv) noexcept
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

std::string SerializeHref(const ada::url_aggregator &url)
{
    auto href = std::string_view(url.get_href());
    absl::ConsumeSuffix(&href, "/");
    return std::string{href};
}

} // namespace

namespace ws {

Expected<Link, LinkError> Link::FromText(const String &text, usize url_bytes_max)
{
    using enum LinkError::Code;

    std::string in{text.View()};
    absl::StripAsciiWhitespace(&in);
    if (in.starts_with("//"))
        return Unex(LinkError{.code = kMissingScheme});
    const auto scheme_pos = in.find("://");
    if (scheme_pos == std::string::npos ||
        !IsValidScheme(std::string_view(in).substr(0, scheme_pos))) {
        in = std::format("http://{}", in);
    } else {
        std::string scheme = in.substr(0, scheme_pos);
        if (!(scheme == "http" || scheme == "https"))
            return Unex(LinkError{.code = kUnsupportedScheme});
    }
    if (unsize(in) > url_bytes_max)
        return Unex(LinkError{.code = kUrlTooLong});
    auto url = ada::parse<ada::url_aggregator>(in);
    if (!url)
        return Unex(LinkError{.code = kFailedToParse});
    auto parsed_url = Url::FromParsed(std::move(*url));
    if (!parsed_url.IsHttpOrHttps())
        return Unex(LinkError{.code = kUnsupportedScheme});
    if (!parsed_url.HasHostname())
        return Unex(LinkError{.code = kMissingHostname});

    if (IsIpLiteralHostname(parsed_url.Hostname()))
        return Unex(LinkError{.code = kIpAddressNotAllowed});

    if (!parsed_url.HasValidDomain())
        return Unex(LinkError{.code = kInvalidHost});

    auto normalized = parsed_url.CopyParsed();
    normalized.set_username("");
    normalized.set_password("");
    normalized.clear_hash();
    normalized.clear_port();

    if (auto hostname = normalized.get_hostname(); !hostname.empty() && hostname.back() == '.')
        normalized.set_hostname(std::string(begin(hostname), end(hostname) - 1));

    return Link{Url::FromParsed(std::move(normalized))};
}

String Link::Host() const { return url.Hostname(); }

String Link::HttpUrl() const
{
    auto copy = url.CopyParsed();
    copy.set_protocol("http");
    return *String::FromBytes(SerializeHref(copy));
}

String Link::HttpsUrl() const
{
    auto copy = url.CopyParsed();
    copy.set_protocol("https");
    return *String::FromBytes(SerializeHref(copy));
}

String Link::Normalized() const
{
    auto copy = url.CopyParsed();
    copy.set_protocol("http");
    constexpr std::string_view http_prefix = "http://";
    return *String::FromBytes(SerializeHref(copy).substr(http_prefix.size()));
}

} // namespace ws
