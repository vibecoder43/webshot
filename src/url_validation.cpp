#include "include/url_validation.hpp"
#include "include/common_definitions.hpp"

#include <cctype>
#include <string_view>

#include <ada.h>
#include <ada/url_aggregator.h>

static std::string trimAsciiWhitespace(std::string s)
{
    auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
    };
    size_t start = 0;
    while (start < s.size() && is_space(s[start]))
        ++start;
    size_t end = s.size();
    while (end > start && is_space(s[end - 1]))
        --end;
    if (start > 0 || end < s.size())
        s = s.substr(start, end - start);
    return s;
}

static bool IsAsciiAlpha(char c) noexcept
{
    const unsigned char u = static_cast<unsigned char>(c);
    return u < 0x80 && std::isalpha(u) != 0;
}
static bool IsAsciiAlnum(char c) noexcept
{
    const unsigned char u = static_cast<unsigned char>(c);
    return u < 0x80 && std::isalnum(u) != 0;
}

// RFC 3986 scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
static bool IsValidScheme(std::string_view sv) noexcept
{
    if (sv.empty() || !IsAsciiAlpha(sv.front()))
        return false;
    for (size_t i = 1; i < sv.size(); ++i) {
        const char c = sv[i];
        if (!(IsAsciiAlnum(c) || c == '+' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

std::string tryNormalizeLink(std::string in, size_t queryPartLengthMax)
{
    // Trim surrounding ASCII whitespace (ASCII-only, safe with UTF-8)
    in = trimAsciiWhitespace(in);
    // Reject network-path reference (starts with "//").
    // Note: ASCII '/' cannot occur inside a UTF-8 code unit, so byte-wise compare is safe.
    if (in.rfind("//", 0) == 0)
        throw InvalidLinkException("missing scheme");

    // If a proper scheme is not present at the start, default to http.
    // We only treat "scheme://" at the beginning as a scheme. Any "://" later
    // in the string is considered part of the path/query.
    const auto scheme_pos = in.find("://");
    if (scheme_pos == std::string::npos ||
        !IsValidScheme(std::string_view(in).substr(0, scheme_pos))) {
        in = std::string("http://") + in;
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
    if (!url->has_valid_domain())
        throw InvalidLinkException("invalid domain");
    if (url->get_search().size() > queryPartLengthMax)
        throw InvalidLinkException("query too long");
    url->set_username("");
    url->set_password("");
    url->clear_hash();
    // Trim a trailing ASCII dot from the host. We intentionally only touch ASCII '.'
    // so that multi-byte Unicode dotted characters (e.g., '。') are not mangled here,
    // leaving canonicalization to ada.
    if (auto hostname = url->get_hostname(); !hostname.empty() && hostname.back() == '.')
        url->set_hostname(std::string(begin(hostname), end(hostname) - 1));
    url->set_protocol("http");
    auto href = std::string(url->get_href().substr(7));
    // Drop a single trailing ASCII '/'. UTF-8 continuation bytes cannot equal '/'.
    if (!href.empty() && href.back() == '/')
        href.pop_back();
    return href;
}
