#include <string>
#include <vector>

#include <userver/utest/utest.hpp>

#include "link.hpp"

namespace {
constexpr size_t kLimit = 1024;

using v1::InvalidLinkException;
using v1::Link;

[[nodiscard]] std::string normalize(std::string input)
{
    auto text = String::fromBytes(std::move(input));
    EXPECT_TRUE(text);
    const auto link = Link::fromText(text.value(), kLimit);
    const auto normalizedText = link.normalized();
    return std::string(normalizedText.view());
}

[[nodiscard]] std::string normalizeBytes(const std::vector<char> &bytes)
{
    std::string input(std::begin(bytes), std::end(bytes));
    auto text = String::fromBytes(std::move(input));
    EXPECT_TRUE(text);
    const auto link = Link::fromText(text.value(), kLimit);
    const auto normalizedText = link.normalized();
    return std::string(normalizedText.view());
}
} // namespace

UTEST(LinkFromUtf8, AcceptsHttpsWithHostname)
{
    auto text = String::fromBytes(std::string{"https://example.com/"});
    ASSERT_TRUE(text);
    const auto link = Link::fromText(text.value(), kLimit);
    EXPECT_EQ(std::string(link.host().view()), std::string{"example.com"});
    EXPECT_EQ(std::string(link.httpUrl().view()), std::string{"http://example.com"});
    EXPECT_EQ(std::string(link.normalized().view()), std::string{"example.com"});
}

UTEST(LinkFromUtf8, RejectsUnsupportedScheme)
{
    EXPECT_THROW(
        {
            auto text = String::fromBytes(std::string{"ftp://example.com/"});
            ASSERT_TRUE(text);
            [[maybe_unused]] auto link = Link::fromText(text.value(), kLimit);
        },
        InvalidLinkException
    );
}

UTEST(LinkFromUtf8, RejectsMissingHostname)
{
    EXPECT_THROW(
        {
            auto text = String::fromBytes(std::string{"http:///"});
            ASSERT_TRUE(text);
            [[maybe_unused]] auto link = Link::fromText(text.value(), kLimit);
        },
        InvalidLinkException
    );
}

UTEST(LinkFromUtf8, AcceptsQueryAtLimit)
{
    std::string urlString = "https://example.com/?";
    urlString.append(kLimit - 1, 'a');
    EXPECT_NO_THROW({
        auto value = normalize(urlString);
        EXPECT_FALSE(value.empty());
    });
}

UTEST(LinkFromUtf8, RejectsQueryOverLimit)
{
    std::string urlString = "https://example.com/?";
    urlString.append(kLimit + 1, 'a');
    EXPECT_THROW({ [[maybe_unused]] auto value = normalize(urlString); }, InvalidLinkException);
}

UTEST(LinkFromUtf8, NormalizesScheme)
{
    const auto httpLink = normalize("http://example.com/path");
    const auto httpsLink = normalize("https://example.com/path");
    EXPECT_EQ(httpLink, httpsLink);
    EXPECT_EQ(httpLink, std::string{"example.com/path"});
}

UTEST(LinkFromUtf8, RemovesTrailingSlash)
{
    EXPECT_EQ(normalize("https://example.com/path/"), std::string{"example.com/path"});
}

UTEST(LinkFromUtf8, TrimsTrailingDotInHost)
{
    EXPECT_EQ(normalize("https://example.com./"), std::string{"example.com"});
}

UTEST(LinkFromUtf8, StripsCredentialsAndFragment)
{
    EXPECT_EQ(normalize("http://user:pass@example.com/a#frag"), std::string{"example.com/a"});
}

UTEST(LinkFromUtf8, AcceptsBareHost)
{
    EXPECT_EQ(normalize("example.com"), std::string{"example.com"});
}

UTEST(LinkFromUtf8, PreservesQueryWithinLimit)
{
    auto link = normalize("https://example.com/?a=1&b=2");
    EXPECT_EQ(link, std::string{"example.com/?a=1&b=2"});
}

UTEST(LinkFromUtf8, RejectsNetworkPathReference)
{
    EXPECT_THROW(
        { [[maybe_unused]] auto value = normalize("//example.com/path"); }, InvalidLinkException
    );
}

UTEST(LinkFromUtf8, RejectsOverlargePort)
{
    EXPECT_THROW(
        { [[maybe_unused]] auto value = normalize("http://example.com:99999/"); },
        InvalidLinkException
    );
}

UTEST(LinkFromUtf8, RejectsIPv6Host)
{
    EXPECT_THROW(
        { [[maybe_unused]] auto value = normalize("http://[::1]/"); }, InvalidLinkException
    );
}

UTEST(LinkFromUtf8, RejectsIPv4Host)
{
    EXPECT_THROW(
        { [[maybe_unused]] auto value = normalize("http://192.0.2.1/"); }, InvalidLinkException
    );
}

UTEST(LinkFromUtf8, KeepsEscapedSlashInPath)
{
    EXPECT_EQ(normalize("https://example.com/a%2Fb"), std::string{"example.com/a%2Fb"});
}

UTEST(LinkFromUtf8, DoesNotMisreadUtf8AsAsciiDelimiters)
{
    // '€' encodes to E2 82 AC in UTF-8 and must not be treated as '/' or '.'
    EXPECT_EQ(normalize("https://example.com/\xE2\x82\xAC"), std::string{"example.com/%E2%82%AC"});
}

UTEST(LinkFromUtf8, TreatsSchemeOnlyAtStart)
{
    // "://" appearing later is part of the path and must not be misparsed as scheme
    EXPECT_EQ(normalize("example.com/http://foo"), std::string{"example.com/http://foo"});
}

UTEST(LinkFromUtf8, DoesNotTrimFullwidthSlash)
{
    // U+FF0F FULLWIDTH SOLIDUS percent-encoded must not be trimmed as a trailing '/'
    EXPECT_EQ(normalize("https://example.com/%EF%BC%8F"), std::string{"example.com/%EF%BC%8F"});
}

UTEST(LinkFromUtf8, AcceptsIDNHostname)
{
    // "bücher.de" should be converted to its punycode form
    EXPECT_EQ(normalize("https://bücher.de/"), std::string{"xn--bcher-kva.de"});
}

UTEST(LinkFromUtf8, TrimsSurroundingWhitespace)
{
    EXPECT_EQ(normalize("  https://example.com/x  "), std::string{"example.com/x"});
}

UTEST(LinkFromUtf8, ResolvesDotSegments)
{
    EXPECT_EQ(normalize("https://example.com/a/./b/../c"), std::string{"example.com/a/c"});
}

UTEST(LinkMembers, HostAndHttpUrlNormalized)
{
    auto text = String::fromBytes(std::string{"https://Example.com:8081/Path/"});
    ASSERT_TRUE(text);
    const auto link = Link::fromTextStripPort(text.value(), kLimit);
    EXPECT_EQ(std::string(link.host().view()), std::string{"example.com"});
    EXPECT_EQ(std::string(link.httpUrl().view()), std::string{"http://example.com/Path"});
    EXPECT_EQ(std::string(link.normalized().view()), std::string{"example.com/Path"});
}

UTEST(LinkFromBytes, MatchesUtf8Normalization)
{
    const std::string url = "https://example.com/a?b=1";
    const auto utf8Normalized = normalize(url);
    const std::vector<char> bytes(std::begin(url), std::end(url));
    const auto bytesNormalized = normalizeBytes(bytes);
    EXPECT_EQ(utf8Normalized, bytesNormalized);
}

UTEST(LinkFromBytes, HandlesUtf8EuroSymbol)
{
    const std::string url = "https://example.com/\xE2\x82\xAC";
    const auto utf8Normalized = normalize(url);
    const std::vector<char> bytes(std::begin(url), std::end(url));
    const auto bytesNormalized = normalizeBytes(bytes);
    EXPECT_EQ(utf8Normalized, bytesNormalized);
}
