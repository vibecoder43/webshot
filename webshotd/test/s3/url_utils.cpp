#include <string>
#include <utility>
#include <vector>

#include <userver/utest/utest.hpp>

#include "s3/url_utils.hpp"
#include "text.hpp"

using ws::s3::DecodeQueryString;
using ws::s3::ParseUrlWithDefaultHttpScheme;
using namespace text::literals;

UTEST(UrlUtils, ParsesUrlWithExistingScheme)
{
    const auto url = ParseUrlWithDefaultHttpScheme("https://example.com/path?a=1"_t);
    ASSERT_TRUE(url);
    EXPECT_EQ(url->Href(), "https://example.com/path?a=1"_t);
}

UTEST(UrlUtils, DefaultsMissingSchemeToHttp)
{
    const auto url = ParseUrlWithDefaultHttpScheme("example.com/path?a=1"_t);
    ASSERT_TRUE(url);
    EXPECT_EQ(url->Href(), "http://example.com/path?a=1"_t);
}

UTEST(UrlUtils, RejectsInvalidUrlEvenAfterDefaultScheme)
{
    EXPECT_FALSE(ParseUrlWithDefaultHttpScheme("https://["_t));
}

UTEST(UrlUtils, EmptyAndQuestionOnly)
{
    {
        const auto v = DecodeQueryString(""_t);
        ASSERT_TRUE(v);
        EXPECT_TRUE(v->empty());
    }
    {
        const auto v = DecodeQueryString("?"_t);
        ASSERT_TRUE(v);
        EXPECT_TRUE(v->empty());
    }
}

UTEST(UrlUtils, SinglePairAndTrailingAmp)
{
    auto v = DecodeQueryString("a=1"_t);
    ASSERT_TRUE(v);
    ASSERT_EQ(v->size(), 1);
    const auto &[key, value] = (*v)[0];
    EXPECT_EQ(key, "a"_t);
    EXPECT_EQ(value, "1"_t);

    v = DecodeQueryString("a=1&"_t);
    ASSERT_TRUE(v);
    ASSERT_EQ(v->size(), 1);
    const auto &[trailing_key, trailing_value] = (*v)[0];
    EXPECT_EQ(trailing_key, "a"_t);
    EXPECT_EQ(trailing_value, "1"_t);
}

UTEST(UrlUtils, MultipleAndRepeatedKeys)
{
    auto v = DecodeQueryString("a=1&b=2&a=3"_t);
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

UTEST(UrlUtils, LeadingQuestionMark)
{
    auto v = DecodeQueryString("?a=1&b=2"_t);
    ASSERT_TRUE(v);
    ASSERT_EQ(v->size(), 2);
    const auto &[key0, value0] = (*v)[0];
    const auto &[key1, value1] = (*v)[1];
    EXPECT_EQ(key0, "a"_t);
    EXPECT_EQ(value0, "1"_t);
    EXPECT_EQ(key1, "b"_t);
    EXPECT_EQ(value1, "2"_t);
}

UTEST(UrlUtils, PercentDecodingKeyAndValue)
{
    auto v = DecodeQueryString("x%20y=hello%20world"_t);
    ASSERT_TRUE(v);
    ASSERT_EQ(v->size(), 1);
    const auto &[key, value] = (*v)[0];
    EXPECT_EQ(key, "x y"_t);
    EXPECT_EQ(value, "hello world"_t);
}

UTEST(UrlUtils, HandlesLeadingSegmentWithoutEquals)
{
    auto v = DecodeQueryString("noeq&foo=bar"_t);
    ASSERT_TRUE(v);
    ASSERT_GE(v->size(), 1);
    const auto &[key, value] = (*v)[0];
    EXPECT_EQ(key, "noeq&foo"_t);
    EXPECT_EQ(value, "bar"_t);
}

UTEST(UrlUtils, AllTextNoEqualsYieldsEmpty)
{
    auto v = DecodeQueryString("noeq"_t);
    ASSERT_TRUE(v);
    EXPECT_TRUE(v->empty());
}
