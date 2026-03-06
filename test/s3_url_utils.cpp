#include <string>
#include <utility>
#include <vector>

#include <userver/utest/utest.hpp>

#include "s3/s3_url_utils.hpp"
#include "text.hpp"

using v1::s3v4::decodeQueryString;
using namespace text::literals;

UTEST(S3UrlUtils, EmptyAndQuestionOnly)
{
    EXPECT_TRUE(decodeQueryString(""_t).empty());
    EXPECT_TRUE(decodeQueryString("?"_t).empty());
}

UTEST(S3UrlUtils, SinglePairAndTrailingAmp)
{
    auto v = decodeQueryString("a=1"_t);
    ASSERT_EQ(v.size(), 1);
    EXPECT_EQ(v[0].first.view(), "a");
    EXPECT_EQ(v[0].second.view(), "1");

    v = decodeQueryString("a=1&"_t);
    ASSERT_EQ(v.size(), 1);
    EXPECT_EQ(v[0].first.view(), "a");
    EXPECT_EQ(v[0].second.view(), "1");
}

UTEST(S3UrlUtils, MultipleAndRepeatedKeys)
{
    auto v = decodeQueryString("a=1&b=2&a=3"_t);
    ASSERT_EQ(v.size(), 3);
    EXPECT_EQ(v[0].first.view(), "a");
    EXPECT_EQ(v[0].second.view(), "1");
    EXPECT_EQ(v[1].first.view(), "b");
    EXPECT_EQ(v[1].second.view(), "2");
    EXPECT_EQ(v[2].first.view(), "a");
    EXPECT_EQ(v[2].second.view(), "3");
}

UTEST(S3UrlUtils, LeadingQuestionMark)
{
    auto v = decodeQueryString("?a=1&b=2"_t);
    ASSERT_EQ(v.size(), 2);
    EXPECT_EQ(v[0].first.view(), "a");
    EXPECT_EQ(v[0].second.view(), "1");
    EXPECT_EQ(v[1].first.view(), "b");
    EXPECT_EQ(v[1].second.view(), "2");
}

UTEST(S3UrlUtils, PercentDecodingKeyAndValue)
{
    auto v = decodeQueryString("x%20y=hello%20world"_t);
    ASSERT_EQ(v.size(), 1);
    EXPECT_EQ(v[0].first.view(), "x y");
    EXPECT_EQ(v[0].second.view(), "hello world");
}

UTEST(S3UrlUtils, HandlesLeadingSegmentWithoutEquals)
{
    auto v = decodeQueryString("noeq&foo=bar"_t);
    ASSERT_GE(v.size(), 1);
    EXPECT_EQ(v[0].first.view(), "noeq&foo");
    EXPECT_EQ(v[0].second.view(), "bar");
}

UTEST(S3UrlUtils, AllTextNoEqualsYieldsEmpty)
{
    auto v = decodeQueryString("noeq"_t);
    EXPECT_TRUE(v.empty());
}
