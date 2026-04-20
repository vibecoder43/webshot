#include <string>
#include <vector>

#include <userver/utest/utest.hpp>

#include "link.hpp"
#include "prefix_utils.hpp"

namespace {
constexpr auto kUrlBytesMax = 4096_uz;

using v1::Link;
using v1::Url;
using enum Url::StripOptions;
using namespace text::literals;

[[nodiscard]] String toText(std::string_view input) { return String::fromBytes(input).expect(); }

[[nodiscard]] Link parseLink(std::string_view input)
{
    return Link::fromText(toText(input), kUrlBytesMax).expect();
}

[[nodiscard]] bool canParseLink(std::string_view input)
{
    return Link::fromText(toText(input), kUrlBytesMax).hasValue();
}

[[nodiscard]] std::string normalizeKey(std::string_view input)
{
    return std::to_string(parseLink(input).normalized());
}

[[nodiscard]] std::string normalizeKeyFromBytes(const std::vector<char> &bytes)
{
    const auto text = String::fromBytes(std::string_view(bytes.data(), bytes.size())).expect();
    return std::to_string(Link::fromText(text, kUrlBytesMax).expect().normalized());
}
} // namespace

UTEST(LinkFromText, AcceptsHttpsWithHostname)
{
    const auto link = parseLink("https://example.com/");
    EXPECT_EQ(link.url.hostname(), "example.com"_t);
    EXPECT_EQ(link.httpUrl(), "http://example.com"_t);
    EXPECT_EQ(link.normalized(), "example.com"_t);
}

UTEST(LinkFromText, RejectsUnsupportedScheme) { EXPECT_FALSE(canParseLink("ftp://example.com/")); }

UTEST(LinkFromText, RejectsMissingHostname) { EXPECT_FALSE(canParseLink("http:///")); }

UTEST(LinkFromText, AcceptsUrlAtLimit)
{
    std::string urlString{"https://example.com/?"};
    ASSERT_LT(urlString.size(), kUrlBytesMax);
    urlString.append(kUrlBytesMax - urlString.size(), 'a');
    EXPECT_NO_THROW({
        auto value = normalizeKey(urlString);
        EXPECT_FALSE(value.empty());
    });
}

UTEST(LinkFromText, RejectsUrlOverLimit)
{
    std::string urlString{"https://example.com/?"};
    ASSERT_LT(urlString.size(), kUrlBytesMax);
    urlString.append(kUrlBytesMax - urlString.size() + 1, 'a');
    EXPECT_FALSE(Link::fromText(toText(urlString), kUrlBytesMax));
}

UTEST(LinkFromText, NormalizesScheme)
{
    const auto httpLink = normalizeKey("http://example.com/path");
    const auto httpsLink = normalizeKey("https://example.com/path");
    EXPECT_EQ(httpLink, httpsLink);
    EXPECT_EQ(httpLink, std::string{"example.com/path"});
}

UTEST(LinkFromText, RemovesTrailingSlash)
{
    EXPECT_EQ(normalizeKey("https://example.com/path/"), std::string{"example.com/path"});
}

UTEST(LinkFromText, TrimsTrailingDotInHost)
{
    EXPECT_EQ(normalizeKey("https://example.com./"), std::string{"example.com"});
}

UTEST(LinkFromText, StripsCredentialsAndFragment)
{
    EXPECT_EQ(normalizeKey("http://user:pass@example.com/a#frag"), std::string{"example.com/a"});
}

UTEST(LinkFromText, AcceptsBareHost)
{
    EXPECT_EQ(normalizeKey("example.com"), std::string{"example.com"});
}

UTEST(LinkFromText, PreservesQueryWithinLimit)
{
    auto link = normalizeKey("https://example.com/?a=1&b=2");
    EXPECT_EQ(link, std::string{"example.com/?a=1&b=2"});
}

UTEST(LinkFromText, RejectsNetworkPathReference)
{
    EXPECT_FALSE(canParseLink("//example.com/path"));
}

UTEST(LinkFromText, RejectsOverlargePort)
{
    EXPECT_FALSE(canParseLink("http://example.com:99999/"));
}

UTEST(LinkFromText, RejectsIPv6Host) { EXPECT_FALSE(canParseLink("http://[::1]/")); }

UTEST(LinkFromText, RejectsIPv4Host) { EXPECT_FALSE(canParseLink("http://192.0.2.1/")); }

UTEST(LinkFromText, KeepsEscapedSlashInPath)
{
    EXPECT_EQ(normalizeKey("https://example.com/a%2Fb"), std::string{"example.com/a%2Fb"});
}

UTEST(LinkFromText, DoesNotMisreadUtf8AsAsciiDelimiters)
{
    // U+20AC encodes to E2 82 AC in UTF-8 and must not be treated as '/' or '.'
    EXPECT_EQ(
        normalizeKey("https://example.com/\xE2\x82\xAC"), std::string{"example.com/%E2%82%AC"}
    );
}

UTEST(LinkFromText, TreatsSchemeOnlyAtStart)
{
    // "://" appearing later is part of the path and must not be misparsed as scheme
    EXPECT_EQ(normalizeKey("example.com/http://foo"), std::string{"example.com/http://foo"});
}

UTEST(LinkFromText, DoesNotTrimFullwidthSlash)
{
    // U+FF0F FULLWIDTH SOLIDUS percent-encoded must not be trimmed as a trailing '/'
    EXPECT_EQ(normalizeKey("https://example.com/%EF%BC%8F"), std::string{"example.com/%EF%BC%8F"});
}

UTEST(LinkFromText, AcceptsIDNHostname)
{
    // "b\303\274cher.de" should be converted to its punycode form
    EXPECT_EQ(normalizeKey("https://b\303\274cher.de/"), std::string{"xn--bcher-kva.de"});
}

UTEST(LinkFromText, TrimsSurroundingWhitespace)
{
    EXPECT_EQ(normalizeKey("  https://example.com/x  "), std::string{"example.com/x"});
}

UTEST(LinkFromText, ResolvesDotSegments)
{
    EXPECT_EQ(normalizeKey("https://example.com/a/./b/../c"), std::string{"example.com/a/c"});
}

UTEST(LinkMembers, HostAndHttpUrlNormalized)
{
    const auto link = parseLink("https://Example.com:8081/Path/");
    EXPECT_EQ(link.url.hostname(), "example.com"_t);
    EXPECT_EQ(link.httpUrl(), "http://example.com/Path"_t);
    EXPECT_EQ(link.normalized(), "example.com/Path"_t);
}

UTEST(LinkMembers, PreservesQueryAndStripsPort)
{
    const auto link = parseLink("https://Example.com:8081/Path?a=1");
    EXPECT_EQ(link.url.hostname(), "example.com"_t);
    EXPECT_EQ(link.normalized(), "example.com/Path?a=1"_t);
}

UTEST(UrlStrip, RemovesPortOnly)
{
    const auto stripped = parseLink("https://example.com:8081/path?a=1").url.stripped(kStripPort);
    EXPECT_EQ(stripped.href(), "https://example.com/path?a=1"_t);
}

UTEST(UrlStrip, RemovesQueryOnly)
{
    const auto stripped = parseLink("https://example.com:8081/path?a=1").url.stripped(kStripQuery);
    EXPECT_EQ(stripped.href(), "https://example.com:8081/path"_t);
}

UTEST(UrlStrip, RemovesPortAndQuery)
{
    const auto stripped =
        parseLink("https://example.com:8081/path?a=1").url.stripped(kStripPort | kStripQuery);
    EXPECT_EQ(stripped.href(), "https://example.com/path"_t);
}

UTEST(PrefixKey, IgnoresPortAndQuery)
{
    EXPECT_EQ(
        v1::prefix::makePrefixKey(parseLink("https://Example.com:8081/Path?a=1")),
        "com.example/Path"_t
    );
}

UTEST(LinkFromTextBytes, MatchesUtf8Normalization)
{
    const std::string url = "https://example.com/a?b=1";
    const auto utf8Normalized = normalizeKey(url);
    const std::vector<char> bytes(std::begin(url), std::end(url));
    const auto bytesNormalized = normalizeKeyFromBytes(bytes);
    EXPECT_EQ(utf8Normalized, bytesNormalized);
}

UTEST(LinkFromTextBytes, HandlesUtf8EuroSymbol)
{
    const std::string url = "https://example.com/\xE2\x82\xAC";
    const auto utf8Normalized = normalizeKey(url);
    const std::vector<char> bytes(std::begin(url), std::end(url));
    const auto bytesNormalized = normalizeKeyFromBytes(bytes);
    EXPECT_EQ(utf8Normalized, bytesNormalized);
}
