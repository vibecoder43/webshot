#include <string>
#include <vector>

#include <userver/utest/utest.hpp>

#include "link.hpp"

namespace {
constexpr size_t kQueryPartLengthMax = 1024UL;

using v1::InvalidLinkException;
using v1::Link;

[[nodiscard]] std::string normalizeKey(std::string_view input)
{
    auto text = String::fromBytes(input);
    if (!text) {
        ADD_FAILURE() << "String::fromBytes failed";
        return {};
    }
    const auto link = Link::fromText(text.value(), kQueryPartLengthMax);
    const auto normalizedText = link.normalized();
    return std::string(normalizedText.view());
}

[[nodiscard]] std::string normalizeKeyFromBytes(const std::vector<char> &bytes)
{
    std::string input(std::begin(bytes), std::end(bytes));
    auto text = String::fromBytes(input);
    if (!text) {
        ADD_FAILURE() << "String::fromBytes failed";
        return {};
    }
    const auto link = Link::fromText(text.value(), kQueryPartLengthMax);
    const auto normalizedText = link.normalized();
    return std::string(normalizedText.view());
}
} // namespace

UTEST(LinkFromText, AcceptsHttpsWithHostname)
{
    auto text = String::fromBytes(std::string{"https://example.com/"});
    ASSERT_TRUE(text);
    if (!text)
        return;
    const auto link = Link::fromText(text.value(), kQueryPartLengthMax);
    EXPECT_EQ(std::string(link.url.hostname().view()), std::string{"example.com"});
    EXPECT_EQ(std::string(link.httpUrl().view()), std::string{"http://example.com"});
    EXPECT_EQ(std::string(link.normalized().view()), std::string{"example.com"});
}

UTEST(LinkFromText, RejectsUnsupportedScheme)
{
    EXPECT_THROW(
        {
            auto text = String::fromBytes(std::string{"ftp://example.com/"});
            if (!text)
                throw std::runtime_error("String::fromBytes failed");
            [[maybe_unused]] auto link = Link::fromText(text.value(), kQueryPartLengthMax);
        },
        InvalidLinkException
    );
}

UTEST(LinkFromText, RejectsMissingHostname)
{
    EXPECT_THROW(
        {
            auto text = String::fromBytes(std::string{"http:///"});
            if (!text)
                throw std::runtime_error("String::fromBytes failed");
            [[maybe_unused]] auto link = Link::fromText(text.value(), kQueryPartLengthMax);
        },
        InvalidLinkException
    );
}

UTEST(LinkFromText, AcceptsQueryAtLimit)
{
    std::string urlString = "https://example.com/?";
    urlString.append(kQueryPartLengthMax - 1, 'a');
    EXPECT_NO_THROW({
        auto value = normalizeKey(urlString);
        EXPECT_FALSE(value.empty());
    });
}

UTEST(LinkFromText, RejectsQueryOverLimit)
{
    std::string urlString = "https://example.com/?";
    urlString.append(kQueryPartLengthMax + 1, 'a');
    EXPECT_THROW({ [[maybe_unused]] auto value = normalizeKey(urlString); }, InvalidLinkException);
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
    EXPECT_THROW(
        { [[maybe_unused]] auto value = normalizeKey("//example.com/path"); }, InvalidLinkException
    );
}

UTEST(LinkFromText, RejectsOverlargePort)
{
    EXPECT_THROW(
        { [[maybe_unused]] auto value = normalizeKey("http://example.com:99999/"); },
        InvalidLinkException
    );
}

UTEST(LinkFromText, RejectsIPv6Host)
{
    EXPECT_THROW(
        { [[maybe_unused]] auto value = normalizeKey("http://[::1]/"); }, InvalidLinkException
    );
}

UTEST(LinkFromText, RejectsIPv4Host)
{
    EXPECT_THROW(
        { [[maybe_unused]] auto value = normalizeKey("http://192.0.2.1/"); }, InvalidLinkException
    );
}

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
    auto text = String::fromBytes(std::string{"https://Example.com:8081/Path/"});
    ASSERT_TRUE(text);
    if (!text)
        return;
    const auto link = Link::fromTextStripPort(text.value(), kQueryPartLengthMax);
    EXPECT_EQ(std::string(link.url.hostname().view()), std::string{"example.com"});
    EXPECT_EQ(std::string(link.httpUrl().view()), std::string{"http://example.com/Path"});
    EXPECT_EQ(std::string(link.normalized().view()), std::string{"example.com/Path"});
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
