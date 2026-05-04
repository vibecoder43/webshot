#include <string>

#include <userver/utils/boost_uuid4.hpp>

#include <userver/utest/utest.hpp>

#include "prefix_pagination.hpp"
#include "text.hpp"

namespace ws {
namespace us = userver;
} // namespace ws

using namespace ws;

using ws::crud::Clock;
using ws::crud::DecodePrefixCursor;
using ws::crud::EncodePrefixCursor;
using ws::crud::PageDirection;
using ws::crud::TimePointToMicros;
using ws::crud::UpperExclusiveBound;
using namespace text::literals;

UTEST(PrefixPagination, UpperExclusiveBoundNormal)
{
    const std::string input = "abc";
    const auto input_text = String::FromBytes(input).Expect();
    const auto upper = UpperExclusiveBound(input_text);
    EXPECT_EQ(upper, "abd");
}

UTEST(PrefixPagination, EncodeDecodeWithoutTimeOrId)
{
    const std::string prefix = "example.com/a";
    const std::string link = "example.com/a/b";

    const auto prefix_text = String::FromBytes(prefix).Expect();
    const auto link_text = String::FromBytes(link).Expect();

    const auto token = EncodePrefixCursor(prefix_text, link_text, PageDirection::kNext);
    const auto decoded = DecodePrefixCursor(token);

    ASSERT_TRUE(decoded);
    if (!decoded)
        return;
    EXPECT_EQ(decoded->prefix, prefix_text);
    EXPECT_EQ(decoded->link, link_text);
    EXPECT_EQ(decoded->direction, PageDirection::kNext);
    EXPECT_FALSE(decoded->created_at);
    EXPECT_FALSE(decoded->id);
}

UTEST(PrefixPagination, EncodeDecodeWithTimeAndIdRoundTrip)
{
    const std::string prefix = "example.com/p/";
    const std::string link = "example.com/p/resource";
    const auto tp = Clock::time_point(std::chrono::microseconds(4242424242));
    const auto id = us::utils::generators::GenerateBoostUuid();
    const auto prefix_text = String::FromBytes(prefix).Expect();
    const auto link_text = String::FromBytes(link).Expect();

    const auto token = EncodePrefixCursor(prefix_text, link_text, tp, id, PageDirection::kPrevious);
    const auto decoded = DecodePrefixCursor(token);

    ASSERT_TRUE(decoded);
    if (!decoded)
        return;
    EXPECT_EQ(decoded->prefix, prefix_text);
    EXPECT_EQ(decoded->link, link_text);
    EXPECT_EQ(decoded->direction, PageDirection::kPrevious);
    ASSERT_TRUE(decoded->created_at);
    ASSERT_TRUE(decoded->id);
    if (!decoded->created_at || !decoded->id)
        return;
    EXPECT_EQ(TimePointToMicros(*decoded->created_at), TimePointToMicros(tp));
    EXPECT_EQ(*decoded->id, id);
}

UTEST(PrefixPagination, DecodeInvalidTokenReturnsNullopt)
{
    const auto decoded = DecodePrefixCursor("invalid-token"_t);
    EXPECT_FALSE(decoded);
}
