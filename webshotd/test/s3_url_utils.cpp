#include <string>
#include <utility>
#include <vector>

#include <userver/utest/utest.hpp>

#include "s3/s3_url_utils.hpp"
#include "text.hpp"

using v1::s3v4::decodeQueryString;
using v1::s3v4::parseUrlWithDefaultHttpScheme;
using namespace text::literals;

UTEST(S3UrlUtils, ParsesUrlWithExistingScheme)
{
    const auto url = parseUrlWithDefaultHttpScheme("https://example.com/path?a=1"_t);
    ASSERT_TRUE(url);
    EXPECT_EQ(url->href(), "https://example.com/path?a=1"_t);
}

UTEST(S3UrlUtils, DefaultsMissingSchemeToHttp)
{
    const auto url = parseUrlWithDefaultHttpScheme("example.com/path?a=1"_t);
    ASSERT_TRUE(url);
    EXPECT_EQ(url->href(), "http://example.com/path?a=1"_t);
}

UTEST(S3UrlUtils, RejectsInvalidUrlEvenAfterDefaultScheme)
{
    EXPECT_FALSE(parseUrlWithDefaultHttpScheme("https://["_t));
}

UTEST(S3UrlUtils, EmptyAndQuestionOnly)
{
    {
        const auto v = decodeQueryString(""_t);
        ASSERT_TRUE(v);
        EXPECT_TRUE(v->empty());
    }
    {
        const auto v = decodeQueryString("?"_t);
        ASSERT_TRUE(v);
        EXPECT_TRUE(v->empty());
    }
}

UTEST(S3UrlUtils, SinglePairAndTrailingAmp)
{
    auto v = decodeQueryString("a=1"_t);
    ASSERT_TRUE(v);
    ASSERT_EQ(v->size(), 1);
    EXPECT_EQ((*v)[0].first, "a"_t);
    EXPECT_EQ((*v)[0].second, "1"_t);

    v = decodeQueryString("a=1&"_t);
    ASSERT_TRUE(v);
    ASSERT_EQ(v->size(), 1);
    EXPECT_EQ((*v)[0].first, "a"_t);
    EXPECT_EQ((*v)[0].second, "1"_t);
}

UTEST(S3UrlUtils, MultipleAndRepeatedKeys)
{
    auto v = decodeQueryString("a=1&b=2&a=3"_t);
    ASSERT_TRUE(v);
    ASSERT_EQ(v->size(), 3);
    EXPECT_EQ((*v)[0].first, "a"_t);
    EXPECT_EQ((*v)[0].second, "1"_t);
    EXPECT_EQ((*v)[1].first, "b"_t);
    EXPECT_EQ((*v)[1].second, "2"_t);
    EXPECT_EQ((*v)[2].first, "a"_t);
    EXPECT_EQ((*v)[2].second, "3"_t);
}

UTEST(S3UrlUtils, LeadingQuestionMark)
{
    auto v = decodeQueryString("?a=1&b=2"_t);
    ASSERT_TRUE(v);
    ASSERT_EQ(v->size(), 2);
    EXPECT_EQ((*v)[0].first, "a"_t);
    EXPECT_EQ((*v)[0].second, "1"_t);
    EXPECT_EQ((*v)[1].first, "b"_t);
    EXPECT_EQ((*v)[1].second, "2"_t);
}

UTEST(S3UrlUtils, PercentDecodingKeyAndValue)
{
    auto v = decodeQueryString("x%20y=hello%20world"_t);
    ASSERT_TRUE(v);
    ASSERT_EQ(v->size(), 1);
    EXPECT_EQ((*v)[0].first, "x y"_t);
    EXPECT_EQ((*v)[0].second, "hello world"_t);
}

UTEST(S3UrlUtils, HandlesLeadingSegmentWithoutEquals)
{
    auto v = decodeQueryString("noeq&foo=bar"_t);
    ASSERT_TRUE(v);
    ASSERT_GE(v->size(), 1);
    EXPECT_EQ((*v)[0].first, "noeq&foo"_t);
    EXPECT_EQ((*v)[0].second, "bar"_t);
}

UTEST(S3UrlUtils, AllTextNoEqualsYieldsEmpty)
{
    auto v = decodeQueryString("noeq"_t);
    ASSERT_TRUE(v);
    EXPECT_TRUE(v->empty());
}
