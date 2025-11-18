#include "include/link.hpp"
/**
 * @file
 * @brief URL normalization and validation using ada.
 *
 * Contains helpers to sanitize user input, enforce scheme/host rules, and
 * produce a stable scheme‑less key for storage and lookups.
 */
#include "include/ip_utils.hpp"

#include <cctype>
#include <string_view>

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

/** RFC 3986 scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) */
static bool isValidScheme(std::string_view sv) noexcept
{
    if (sv.empty() || !isAsciiAlpha(sv.front()))
        return false;
    for (size_t i = 1; i < sv.size(); ++i) {
        const char c = sv[i];
        if (!(isAsciiAlnum(c) || c == '+' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

/** Build a scheme‑less canonical form for indexing. */
static std::string buildSchemeLess(ada::url_aggregator &url)
{
    url.set_protocol("http");
    auto href = std::string(url.get_href().substr(7));
    if (!href.empty() && href.back() == '/')
        href.pop_back();
    return href;
}

} // namespace

namespace v1 {

Link Link::fromUserInput(std::string in, size_t queryPartLengthMax)
{
    absl::StripAsciiWhitespace(&in);
    if (in.rfind("//", 0) == 0)
        throw InvalidLinkException("missing scheme");

    const auto scheme_pos = in.find("://");
    if (scheme_pos == std::string::npos ||
        !isValidScheme(std::string_view(in).substr(0, scheme_pos))) {
        in = fmt::format("http://{}", in);
    } else {
        std::string scheme = in.substr(0, scheme_pos);
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

    if (IpUtils::isIpLiteralHostname(url->get_hostname()))
        throw InvalidLinkException("ip address not allowed");

    if (!url->has_valid_domain())
        throw InvalidLinkException("invalid host");
    if (url->get_search().size() > queryPartLengthMax)
        throw InvalidLinkException("query too long");

    url->set_username("");
    url->set_password("");
    url->clear_hash();
    if (auto hostname = url->get_hostname(); !hostname.empty() && hostname.back() == '.')
        url->set_hostname(std::string(begin(hostname), end(hostname) - 1));

    Link out;
    out.url = std::move(*url);
    out.schemeLess = buildSchemeLess(out.url);
    return out;
}

} // namespace v1
