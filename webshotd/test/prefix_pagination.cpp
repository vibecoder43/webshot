#include <string>

#include <userver/utils/boost_uuid4.hpp>

#include <userver/utest/utest.hpp>

#include "prefix_pagination.hpp"
#include "text.hpp"

using v1::crud::Clock;
using v1::crud::decodePrefixCursor;
using v1::crud::encodePrefixCursor;
using v1::crud::timePointToMicros;
using v1::crud::upperExclusiveBound;
using namespace text::literals;

UTEST(PrefixPagination, UpperExclusiveBoundNormal)
{
    const std::string input = "abc";
    const auto inputText = String::fromBytesThrow(input);
    const auto upper = upperExclusiveBound(inputText);
    EXPECT_EQ(upper, std::string{"abd"});
}

UTEST(PrefixPagination, EncodeDecodeWithoutTimeOrId)
{
    const std::string prefix = "example.com/a";
    const std::string link = "example.com/a/b";

    const auto prefixText = String::fromBytesThrow(prefix);
    const auto linkText = String::fromBytesThrow(link);

    const auto token = encodePrefixCursor(prefixText, linkText);
    const auto decoded = decodePrefixCursor(token);

    ASSERT_TRUE(decoded);
    if (!decoded)
        return;
    EXPECT_EQ(std::string(decoded->prefix.view()), prefix);
    EXPECT_EQ(std::string(decoded->link.view()), link);
    EXPECT_FALSE(decoded->createdAt);
    EXPECT_FALSE(decoded->id);
}

UTEST(PrefixPagination, EncodeDecodeWithTimeAndIdRoundTrip)
{
    const std::string prefix = "example.com/p/";
    const std::string link = "example.com/p/resource";
    const auto tp = Clock::time_point(std::chrono::microseconds(4242424242));
    const auto id = userver::utils::generators::GenerateBoostUuid();
    const auto prefixText = String::fromBytesThrow(prefix);
    const auto linkText = String::fromBytesThrow(link);

    const auto token = encodePrefixCursor(prefixText, linkText, tp, id);
    const auto decoded = decodePrefixCursor(token);

    ASSERT_TRUE(decoded);
    if (!decoded)
        return;
    EXPECT_EQ(std::string(decoded->prefix.view()), prefix);
    EXPECT_EQ(std::string(decoded->link.view()), link);
    ASSERT_TRUE(decoded->createdAt);
    ASSERT_TRUE(decoded->id);
    if (!decoded->createdAt || !decoded->id)
        return;
    EXPECT_EQ(timePointToMicros(decoded->createdAt.value()), timePointToMicros(tp));
    EXPECT_EQ(decoded->id.value(), id);
}

UTEST(PrefixPagination, DecodeInvalidTokenReturnsNullopt)
{
    const auto decoded = decodePrefixCursor("invalid-token"_t);
    EXPECT_FALSE(decoded);
}
