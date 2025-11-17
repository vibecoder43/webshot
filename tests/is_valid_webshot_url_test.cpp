#include <string>

#include <userver/utest/utest.hpp>

#include "link.hpp"

namespace {
constexpr size_t kLimit = 1024;

using v1::InvalidLinkException;
using v1::Link;

[[nodiscard]] std::string normalize(std::string input)
{
    return Link::fromUserInput(std::move(input), kLimit).normalized();
}
} // namespace

UTEST(LinkFromUserInput, AcceptsHttpsWithHostname)
{
    const auto link = Link::fromUserInput("https://example.com/", kLimit);
    EXPECT_EQ(link.host(), std::string{"example.com"});
    EXPECT_EQ(link.httpUrl(), std::string{"http://example.com"});
    EXPECT_EQ(link.normalized(), std::string{"example.com"});
}

UTEST(LinkFromUserInput, RejectsUnsupportedScheme)
{
    EXPECT_THROW(
        { [[maybe_unused]] auto link = Link::fromUserInput("ftp://example.com/", kLimit); },
        InvalidLinkException
    );
}

UTEST(LinkFromUserInput, RejectsMissingHostname)
{
    EXPECT_THROW(
        { [[maybe_unused]] auto link = Link::fromUserInput("http:///", kLimit); },
        InvalidLinkException
    );
}

UTEST(LinkFromUserInput, AcceptsQueryAtLimit)
{
    std::string url_string = "https://example.com/?";
    url_string.append(kLimit - 1, 'a');
    EXPECT_NO_THROW({
        auto value = normalize(url_string);
        EXPECT_FALSE(value.empty());
    });
}

UTEST(LinkFromUserInput, RejectsQueryOverLimit)
{
    std::string url_string = "https://example.com/?";
    url_string.append(kLimit + 1, 'a');
    EXPECT_THROW({ [[maybe_unused]] auto value = normalize(url_string); }, InvalidLinkException);
}

UTEST(LinkFromUserInput, NormalizesScheme)
{
    const auto http_link = normalize("http://example.com/path");
    const auto https_link = normalize("https://example.com/path");
    EXPECT_EQ(http_link, https_link);
    EXPECT_EQ(http_link, std::string{"example.com/path"});
}

UTEST(LinkFromUserInput, RemovesTrailingSlash)
{
    EXPECT_EQ(normalize("https://example.com/path/"), std::string{"example.com/path"});
}

UTEST(LinkFromUserInput, TrimsTrailingDotInHost)
{
    EXPECT_EQ(normalize("https://example.com./"), std::string{"example.com"});
}

UTEST(LinkFromUserInput, StripsCredentialsAndFragment)
{
    EXPECT_EQ(normalize("http://user:pass@example.com/a#frag"), std::string{"example.com/a"});
}

UTEST(LinkFromUserInput, AcceptsBareHost)
{
    EXPECT_EQ(normalize("example.com"), std::string{"example.com"});
}

UTEST(LinkFromUserInput, PreservesQueryWithinLimit)
{
    auto link = normalize("https://example.com/?a=1&b=2");
    EXPECT_EQ(link, std::string{"example.com/?a=1&b=2"});
}

UTEST(LinkFromUserInput, RejectsNetworkPathReference)
{
    EXPECT_THROW(
        { [[maybe_unused]] auto value = normalize("//example.com/path"); }, InvalidLinkException
    );
}

UTEST(LinkFromUserInput, RejectsOverlargePort)
{
    EXPECT_THROW(
        { [[maybe_unused]] auto value = normalize("http://example.com:99999/"); },
        InvalidLinkException
    );
}

UTEST(LinkFromUserInput, RejectsIPv6Host)
{
    EXPECT_THROW(
        { [[maybe_unused]] auto value = normalize("http://[::1]/"); }, InvalidLinkException
    );
}

UTEST(LinkFromUserInput, RejectsIPv4Host)
{
    EXPECT_THROW(
        { [[maybe_unused]] auto value = normalize("http://192.0.2.1/"); }, InvalidLinkException
    );
}

UTEST(LinkFromUserInput, KeepsEscapedSlashInPath)
{
    EXPECT_EQ(normalize("https://example.com/a%2Fb"), std::string{"example.com/a%2Fb"});
}

UTEST(LinkFromUserInput, DoesNotMisreadUtf8AsAsciiDelimiters)
{
    // '€' encodes to E2 82 AC in UTF-8 and must not be treated as '/' or '.'
    EXPECT_EQ(normalize("https://example.com/\xE2\x82\xAC"), std::string{"example.com/%E2%82%AC"});
}

UTEST(LinkFromUserInput, TreatsSchemeOnlyAtStart)
{
    // "://" appearing later is part of the path and must not be misparsed as scheme
    EXPECT_EQ(normalize("example.com/http://foo"), std::string{"example.com/http://foo"});
}

UTEST(LinkFromUserInput, DoesNotTrimFullwidthSlash)
{
    // U+FF0F FULLWIDTH SOLIDUS percent-encoded must not be trimmed as a trailing '/'
    EXPECT_EQ(normalize("https://example.com/%EF%BC%8F"), std::string{"example.com/%EF%BC%8F"});
}

UTEST(LinkFromUserInput, AcceptsIDNHostname)
{
    // "bücher.de" should be converted to its punycode form
    EXPECT_EQ(normalize("https://bücher.de/"), std::string{"xn--bcher-kva.de"});
}

UTEST(LinkFromUserInput, TrimsSurroundingWhitespace)
{
    EXPECT_EQ(normalize("  https://example.com/x  "), std::string{"example.com/x"});
}

UTEST(LinkFromUserInput, ResolvesDotSegments)
{
    EXPECT_EQ(normalize("https://example.com/a/./b/../c"), std::string{"example.com/a/c"});
}

UTEST(LinkMembers, HostAndHttpUrlNormalized)
{
    const auto link = Link::fromUserInput("https://Example.com/Path/", kLimit);
    EXPECT_EQ(link.host(), std::string{"example.com"});
    EXPECT_EQ(link.httpUrl(), std::string{"http://example.com/Path"});
    EXPECT_EQ(link.normalized(), std::string{"example.com/Path"});
}
