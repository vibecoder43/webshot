#include <string>

#include <string>

#include <userver/utest/utest.hpp>

#include "url_validation.hpp"

namespace {
constexpr size_t kLimit = 1024;
}

UTEST(TryNormalizeLink, AcceptsHttpsWithHostname)
{
    EXPECT_NO_THROW({ auto link = tryNormalizeLink("https://example.com/", kLimit); });
}

UTEST(TryNormalizeLink, RejectsUnsupportedScheme)
{
    EXPECT_THROW(tryNormalizeLink("ftp://example.com/", kLimit), InvalidLinkException);
}

UTEST(TryNormalizeLink, RejectsMissingHostname)
{
    EXPECT_THROW(tryNormalizeLink("http:///", kLimit), InvalidLinkException);
}

UTEST(TryNormalizeLink, AcceptsQueryAtLimit)
{
    std::string url_string = "https://example.com/?";
    url_string.append(kLimit - 1, 'a');
    EXPECT_NO_THROW(tryNormalizeLink(url_string, kLimit));
}

UTEST(TryNormalizeLink, RejectsQueryOverLimit)
{
    std::string url_string = "https://example.com/?";
    url_string.append(kLimit + 1, 'a');
    EXPECT_THROW(tryNormalizeLink(url_string, kLimit), InvalidLinkException);
}

UTEST(TryNormalizeLink, NormalizesScheme)
{
    const auto http_link = tryNormalizeLink("http://example.com/path", kLimit);
    const auto https_link = tryNormalizeLink("https://example.com/path", kLimit);
    EXPECT_EQ(http_link, https_link);
    EXPECT_EQ(http_link, std::string{"example.com/path"});
}

UTEST(TryNormalizeLink, RemovesTrailingSlash)
{
    EXPECT_EQ(
        tryNormalizeLink("https://example.com/path/", kLimit), std::string{"example.com/path"}
    );
}

UTEST(TryNormalizeLink, TrimsTrailingDotInHost)
{
    EXPECT_EQ(tryNormalizeLink("https://example.com./", kLimit), std::string{"example.com"});
}

UTEST(TryNormalizeLink, StripsCredentialsAndFragment)
{
    EXPECT_EQ(
        tryNormalizeLink("http://user:pass@example.com/a#frag", kLimit),
        std::string{"example.com/a"}
    );
}

UTEST(TryNormalizeLink, AcceptsBareHost)
{
    EXPECT_EQ(tryNormalizeLink("example.com", kLimit), std::string{"example.com"});
}

UTEST(TryNormalizeLink, PreservesQueryWithinLimit)
{
    auto link = tryNormalizeLink("https://example.com/?a=1&b=2", kLimit);
    EXPECT_EQ(link, std::string{"example.com/?a=1&b=2"});
}

UTEST(TryNormalizeLink, RejectsNetworkPathReference)
{
    EXPECT_THROW(tryNormalizeLink("//example.com/path", kLimit), InvalidLinkException);
}

UTEST(TryNormalizeLink, RejectsOverlargePort)
{
    EXPECT_THROW(tryNormalizeLink("http://example.com:99999/", kLimit), InvalidLinkException);
}

UTEST(TryNormalizeLink, AcceptsIPv6Host)
{
    EXPECT_EQ(tryNormalizeLink("http://[::1]/", kLimit), std::string{"[::1]"});
}

UTEST(TryNormalizeLink, KeepsEscapedSlashInPath)
{
    EXPECT_EQ(
        tryNormalizeLink("https://example.com/a%2Fb", kLimit), std::string{"example.com/a%2Fb"}
    );
}

UTEST(TryNormalizeLink, DoesNotMisreadUtf8AsAsciiDelimiters)
{
    // '€' encodes to E2 82 AC in UTF-8 and must not be treated as '/' or '.'
    EXPECT_EQ(
        tryNormalizeLink("https://example.com/\xE2\x82\xAC", kLimit),
        std::string{"example.com/%E2%82%AC"}
    );
}

UTEST(TryNormalizeLink, TreatsSchemeOnlyAtStart)
{
    // "://" appearing later is part of the path and must not be misparsed as scheme
    EXPECT_EQ(
        tryNormalizeLink("example.com/http://foo", kLimit), std::string{"example.com/http://foo"}
    );
}

UTEST(TryNormalizeLink, DoesNotTrimFullwidthSlash)
{
    // U+FF0F FULLWIDTH SOLIDUS percent-encoded must not be trimmed as a trailing '/'
    EXPECT_EQ(
        tryNormalizeLink("https://example.com/%EF%BC%8F", kLimit),
        std::string{"example.com/%EF%BC%8F"}
    );
}

UTEST(TryNormalizeLink, AcceptsIDNHostname)
{
    // "bücher.de" should be converted to its punycode form
    EXPECT_EQ(tryNormalizeLink("https://bücher.de/", kLimit), std::string{"xn--bcher-kva.de"});
}

UTEST(TryNormalizeLink, TrimsSurroundingWhitespace)
{
    EXPECT_EQ(tryNormalizeLink("  https://example.com/x  ", kLimit), std::string{"example.com/x"});
}

UTEST(TryNormalizeLink, ResolvesDotSegments)
{
    EXPECT_EQ(
        tryNormalizeLink("https://example.com/a/./b/../c", kLimit), std::string{"example.com/a/c"}
    );
}
