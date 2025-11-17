#include <string>

#include <boost/uuid/random_generator.hpp>

#include <userver/utest/utest.hpp>

#include "webshot_prefix_pagination.hpp"

using v1::crud::Clock;
using v1::crud::decodePrefixCursor;
using v1::crud::encodePrefixCursor;
using v1::crud::PrefixCursor;
using v1::crud::timePointToMicros;
using v1::crud::upperExclusiveBound;

UTEST(WebshotPrefixPagination, UpperExclusiveBoundNormal)
{
    const std::string input = "abc";
    const auto upper = upperExclusiveBound(input);
    ASSERT_TRUE(upper);
    EXPECT_EQ(*upper, std::string{"abd"});
}

UTEST(WebshotPrefixPagination, UpperExclusiveBoundAllFFReturnsNullopt)
{
    const std::string input(3, '\xFF');
    const auto upper = upperExclusiveBound(input);
    EXPECT_FALSE(upper.has_value());
}

UTEST(WebshotPrefixPagination, EncodeDecodeWithoutTimeOrId)
{
    const std::string prefix = "example.com/a";
    const std::string link = "example.com/a/b";

    const auto token = encodePrefixCursor(prefix, link);
    const auto decoded = decodePrefixCursor(token);

    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->prefix, prefix);
    EXPECT_EQ(decoded->link, link);
    EXPECT_FALSE(decoded->createdAt);
    EXPECT_FALSE(decoded->id);
}

UTEST(WebshotPrefixPagination, EncodeDecodeWithTimeAndIdRoundTrip)
{
    const std::string prefix = "example.com/p/";
    const std::string link = "example.com/p/resource";
    const auto tp = Clock::time_point(std::chrono::microseconds(4242424242));
    const auto id = boost::uuids::random_generator()();

    const auto token = encodePrefixCursor(prefix, link, tp, id);
    const auto decoded = decodePrefixCursor(token);

    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->prefix, prefix);
    EXPECT_EQ(decoded->link, link);
    ASSERT_TRUE(decoded->createdAt);
    ASSERT_TRUE(decoded->id);
    EXPECT_EQ(timePointToMicros(*decoded->createdAt), timePointToMicros(tp));
    EXPECT_EQ(*decoded->id, id);
}

UTEST(WebshotPrefixPagination, DecodeInvalidTokenReturnsNullopt)
{
    const auto decoded = decodePrefixCursor("invalid-token");
    EXPECT_FALSE(decoded);
}
