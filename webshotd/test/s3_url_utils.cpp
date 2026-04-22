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
    const auto &[key, value] = (*v)[0];
    EXPECT_EQ(key, "a"_t);
    EXPECT_EQ(value, "1"_t);

    v = decodeQueryString("a=1&"_t);
    ASSERT_TRUE(v);
    ASSERT_EQ(v->size(), 1);
    const auto &[trailingKey, trailingValue] = (*v)[0];
    EXPECT_EQ(trailingKey, "a"_t);
    EXPECT_EQ(trailingValue, "1"_t);
}

UTEST(S3UrlUtils, MultipleAndRepeatedKeys)
{
    auto v = decodeQueryString("a=1&b=2&a=3"_t);
    ASSERT_TRUE(v);
    ASSERT_EQ(v->size(), 3);
    const auto &[key0, value0] = (*v)[0];
    const auto &[key1, value1] = (*v)[1];
    const auto &[key2, value2] = (*v)[2];
    EXPECT_EQ(key0, "a"_t);
    EXPECT_EQ(value0, "1"_t);
    EXPECT_EQ(key1, "b"_t);
    EXPECT_EQ(value1, "2"_t);
    EXPECT_EQ(key2, "a"_t);
    EXPECT_EQ(value2, "3"_t);
}

UTEST(S3UrlUtils, LeadingQuestionMark)
{
    auto v = decodeQueryString("?a=1&b=2"_t);
    ASSERT_TRUE(v);
    ASSERT_EQ(v->size(), 2);
    const auto &[key0, value0] = (*v)[0];
    const auto &[key1, value1] = (*v)[1];
    EXPECT_EQ(key0, "a"_t);
    EXPECT_EQ(value0, "1"_t);
    EXPECT_EQ(key1, "b"_t);
    EXPECT_EQ(value1, "2"_t);
}

UTEST(S3UrlUtils, PercentDecodingKeyAndValue)
{
    auto v = decodeQueryString("x%20y=hello%20world"_t);
    ASSERT_TRUE(v);
    ASSERT_EQ(v->size(), 1);
    const auto &[key, value] = (*v)[0];
    EXPECT_EQ(key, "x y"_t);
    EXPECT_EQ(value, "hello world"_t);
}

UTEST(S3UrlUtils, HandlesLeadingSegmentWithoutEquals)
{
    auto v = decodeQueryString("noeq&foo=bar"_t);
    ASSERT_TRUE(v);
    ASSERT_GE(v->size(), 1);
    const auto &[key, value] = (*v)[0];
    EXPECT_EQ(key, "noeq&foo"_t);
    EXPECT_EQ(value, "bar"_t);
}

UTEST(S3UrlUtils, AllTextNoEqualsYieldsEmpty)
{
    auto v = decodeQueryString("noeq"_t);
    ASSERT_TRUE(v);
    EXPECT_TRUE(v->empty());
}
