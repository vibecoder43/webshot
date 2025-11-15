#include "include/link.hpp"
#include "include/ip_utils.hpp"

#include <cctype>
#include <string_view>

#include <ada.h>
#include <ada/url_aggregator.h>

namespace {

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

static bool isAsciiAlpha(char c) noexcept
{
    const unsigned char u = static_cast<unsigned char>(c);
    return u < 0x80 && std::isalpha(u) != 0;
}
static bool isAsciiAlnum(char c) noexcept
{
    const unsigned char u = static_cast<unsigned char>(c);
    return u < 0x80 && std::isalnum(u) != 0;
}

// RFC 3986 scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
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
    in = trimAsciiWhitespace(std::move(in));
    if (in.rfind("//", 0) == 0)
        throw InvalidLinkException("missing scheme");

    const auto scheme_pos = in.find("://");
    if (scheme_pos == std::string::npos ||
        !isValidScheme(std::string_view(in).substr(0, scheme_pos))) {
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

    if (IpUtils::isIpLiteralHostname(url->get_hostname()))
        throw InvalidLinkException("ip address not allowed");

    if (!url->has_valid_domain())
        throw InvalidLinkException("invalid domain");
    if (url->get_search().size() > queryPartLengthMax)
        throw InvalidLinkException("query too long");

    url->set_username("");
    url->set_password("");
    url->clear_hash();
    if (auto hostname = url->get_hostname(); !hostname.empty() && hostname.back() == '.')
        url->set_hostname(std::string(begin(hostname), end(hostname) - 1));

    Link out;
    out.url = std::move(*url);
    out.scheme_less = buildSchemeLess(out.url);
    return out;
}

} // namespace v1
