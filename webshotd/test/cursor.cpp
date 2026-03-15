#include <string>

#include <userver/utils/boost_uuid4.hpp>

#include <userver/utest/utest.hpp>

#include "cursor.hpp"
#include "schema/webshot.hpp"
#include "text.hpp"

using v1::crud::Clock;
using v1::crud::decodeToken;
using v1::crud::encodeToken;
using v1::crud::microsToTimePoint;
using v1::crud::timePointToMicros;
using namespace text::literals;

UTEST(Cursor, TimePointRoundTrip)
{
    const auto tp = Clock::now();
    const auto micros = timePointToMicros(tp);
    const auto tp2 = microsToTimePoint(micros);
    EXPECT_EQ(timePointToMicros(tp2), micros);
}

UTEST(Cursor, EncodeDecodePaginationCursor)
{
    const auto tp = Clock::time_point(std::chrono::microseconds(123456789));
    const auto micros = timePointToMicros(tp);
    const auto id = userver::utils::generators::GenerateBoostUuid();

    dto::PaginationCursor cur(micros, id);
    const auto token = encodeToken(cur);

    const auto decoded = decodeToken<dto::PaginationCursor>(token);
    ASSERT_TRUE(decoded);
    if (!decoded)
        return;
    EXPECT_EQ(decoded->t, micros);
    EXPECT_EQ(decoded->i, id);
}

UTEST(Cursor, DecodeTokenInvalidReturnsNullopt)
{
    const auto decoded = decodeToken<dto::PaginationCursor>("not-a-token"_t);
    EXPECT_FALSE(decoded);
}
