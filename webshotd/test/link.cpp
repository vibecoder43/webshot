#include <string>
#include <vector>

#include <userver/utest/utest.hpp>

#include "link.hpp"
#include "prefix_utils.hpp"

namespace {
constexpr auto kUrlBytesMax = 4096_uz;

using ws::Link;
using ws::Url;
using enum Url::StripOptions;
using namespace text::literals;

[[nodiscard]] String ToText(std::string_view input) { return *String::FromBytes(input); }

[[nodiscard]] Link ParseLink(std::string_view input)
{
    return *Link::FromText(ToText(input), kUrlBytesMax);
}

[[nodiscard]] Url ParseUrl(std::string_view input) { return Url::FromText(ToText(input)).value(); }

[[nodiscard]] bool CanParseLink(std::string_view input)
{
    return Link::FromText(ToText(input), kUrlBytesMax).HasValue();
}

[[nodiscard]] std::string NormalizeKey(std::string_view input)
{
    return ParseLink(input).ToKey().ToBytes();
}

[[nodiscard]] std::string NormalizeKeyFromBytes(const std::vector<char> &bytes)
{
    auto text = *String::FromBytes(std::string_view(bytes.data(), bytes.size()));
    return Link::FromText(text, kUrlBytesMax)->ToKey().ToBytes();
}
} // namespace

UTEST(LinkFromText, AcceptsHttpsWithHostname)
{
    auto link = ParseLink("https://example.com/");
    EXPECT_EQ(link.Hostname(), "example.com"_t);
    EXPECT_EQ(link.HttpUrl(), "http://example.com"_t);
    EXPECT_EQ(link.ToKey(), "example.com"_t);
}

UTEST(LinkFromText, RejectsUnsupportedScheme) { EXPECT_FALSE(CanParseLink("ftp://example.com/")); }

UTEST(LinkFromText, RejectsMissingHostname) { EXPECT_FALSE(CanParseLink("http:///")); }

UTEST(LinkFromText, AcceptsUrlAtLimit)
{
    std::string url_string{"https://example.com/?"};
    ASSERT_LT(url_string.size(), kUrlBytesMax);
    url_string.append(kUrlBytesMax - url_string.size(), 'a');
    EXPECT_NO_THROW({
        auto value = NormalizeKey(url_string);
        EXPECT_FALSE(value.empty());
    });
}

UTEST(LinkFromText, RejectsUrlOverLimit)
{
    std::string url_string{"https://example.com/?"};
    ASSERT_LT(url_string.size(), kUrlBytesMax);
    url_string.append(kUrlBytesMax - url_string.size() + 1, 'a');
    EXPECT_FALSE(Link::FromText(ToText(url_string), kUrlBytesMax));
}

UTEST(LinkFromText, NormalizesScheme)
{
    auto http_link = NormalizeKey("http://example.com/path");
    auto https_link = NormalizeKey("https://example.com/path");
    EXPECT_EQ(http_link, https_link);
    EXPECT_EQ(http_link, std::string{"example.com/path"});
}

UTEST(LinkFromText, RemovesTrailingSlash)
{
    EXPECT_EQ(NormalizeKey("https://example.com/path/"), std::string{"example.com/path"});
}

UTEST(LinkFromText, TrimsTrailingDotInHost)
{
    EXPECT_EQ(NormalizeKey("https://example.com./"), std::string{"example.com"});
}

UTEST(LinkFromText, StripsCredentialsAndFragment)
{
    EXPECT_EQ(NormalizeKey("http://user:pass@example.com/a#frag"), std::string{"example.com/a"});
}

UTEST(LinkFromText, AcceptsBareHost)
{
    EXPECT_EQ(NormalizeKey("example.com"), std::string{"example.com"});
}

UTEST(LinkFromText, PreservesQueryWithinLimit)
{
    auto link = NormalizeKey("https://example.com/?a=1&b=2");
    EXPECT_EQ(link, std::string{"example.com/?a=1&b=2"});
}

UTEST(LinkFromText, RejectsNetworkPathReference)
{
    EXPECT_FALSE(CanParseLink("//example.com/path"));
}

UTEST(LinkFromText, RejectsOverlargePort)
{
    EXPECT_FALSE(CanParseLink("http://example.com:99999/"));
}

UTEST(LinkFromText, RejectsIPv6Host) { EXPECT_FALSE(CanParseLink("http://[::1]/")); }

UTEST(LinkFromText, RejectsIPv4Host) { EXPECT_FALSE(CanParseLink("http://192.0.2.1/")); }

UTEST(LinkFromText, RejectsPublicIPv4Host) { EXPECT_FALSE(CanParseLink("http://8.8.8.8/")); }

UTEST(LinkFromText, KeepsEscapedSlashInPath)
{
    EXPECT_EQ(NormalizeKey("https://example.com/a%2Fb"), std::string{"example.com/a%2Fb"});
}

UTEST(LinkFromText, DoesNotMisreadUtf8AsAsciiDelimiters)
{
    // U+20AC encodes to E2 82 AC in UTF-8 and must not be treated as '/' or '.'
    EXPECT_EQ(
        NormalizeKey("https://example.com/\xE2\x82\xAC"), std::string{"example.com/%E2%82%AC"}
    );
}

UTEST(LinkFromText, TreatsSchemeOnlyAtStart)
{
    // "://" appearing later is part of the path and must not be misparsed as scheme
    EXPECT_EQ(NormalizeKey("example.com/http://foo"), std::string{"example.com/http://foo"});
}

UTEST(LinkFromText, DoesNotTrimFullwidthSlash)
{
    // U+FF0F FULLWIDTH SOLIDUS percent-encoded must not be trimmed as a trailing '/'
    EXPECT_EQ(NormalizeKey("https://example.com/%EF%BC%8F"), std::string{"example.com/%EF%BC%8F"});
}

UTEST(LinkFromText, AcceptsIDNHostname)
{
    // "b\303\274cher.de" should be converted to its punycode form
    EXPECT_EQ(NormalizeKey("https://b\303\274cher.de/"), std::string{"xn--bcher-kva.de"});
}

UTEST(LinkFromText, TrimsSurroundingWhitespace)
{
    EXPECT_EQ(NormalizeKey("  https://example.com/x  "), std::string{"example.com/x"});
}

UTEST(LinkFromText, ResolvesDotSegments)
{
    EXPECT_EQ(NormalizeKey("https://example.com/a/./b/../c"), std::string{"example.com/a/c"});
}

UTEST(LinkMembers, HostAndHttpUrlToKey)
{
    auto link = ParseLink("https://Example.com:8081/Path/");
    EXPECT_EQ(link.Hostname(), "example.com"_t);
    EXPECT_EQ(link.HttpUrl(), "http://example.com/Path"_t);
    EXPECT_EQ(link.ToKey(), "example.com/Path"_t);
}

UTEST(LinkMembers, PreservesQueryAndStripsPort)
{
    auto link = ParseLink("https://Example.com:8081/Path?a=1");
    EXPECT_EQ(link.Hostname(), "example.com"_t);
    EXPECT_EQ(link.ToKey(), "example.com/Path?a=1"_t);
}

UTEST(UrlStrip, RemovesPortOnly)
{
    auto stripped = ParseUrl("https://example.com:8081/path?a=1").Without(kPort);
    EXPECT_EQ(stripped.Href(), "https://example.com/path?a=1"_t);
}

UTEST(UrlStrip, RemovesQueryOnly)
{
    auto stripped = ParseUrl("https://example.com:8081/path?a=1").Without(kQuery);
    EXPECT_EQ(stripped.Href(), "https://example.com:8081/path"_t);
}

UTEST(UrlStrip, RemovesPortAndQuery)
{
    auto stripped = ParseUrl("https://example.com:8081/path?a=1").Without(kPort | kQuery);
    EXPECT_EQ(stripped.Href(), "https://example.com/path"_t);
}

UTEST(PrefixKey, IgnoresPortAndQuery)
{
    EXPECT_EQ(
        ws::prefix::MakePrefixKey(ParseLink("https://Example.com:8081/Path?a=1")),
        "com.example/Path"_t
    );
}

UTEST(LinkFromTextBytes, MatchesUtf8Normalization)
{
    const std::string url = "https://example.com/a?b=1";
    auto utf8_normalized = NormalizeKey(url);
    const std::vector<char> bytes(std::begin(url), std::end(url));
    auto bytes_normalized = NormalizeKeyFromBytes(bytes);
    EXPECT_EQ(utf8_normalized, bytes_normalized);
}

UTEST(LinkFromTextBytes, HandlesUtf8EuroSymbol)
{
    const std::string url = "https://example.com/\xE2\x82\xAC";
    auto utf8_normalized = NormalizeKey(url);
    const std::vector<char> bytes(std::begin(url), std::end(url));
    auto bytes_normalized = NormalizeKeyFromBytes(bytes);
    EXPECT_EQ(utf8_normalized, bytes_normalized);
}
