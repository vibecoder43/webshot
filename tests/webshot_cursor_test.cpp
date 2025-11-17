#include <string>

#include <boost/uuid/random_generator.hpp>

#include <userver/utest/utest.hpp>

#include "schemas/webshot.hpp"
#include "webshot_cursor.hpp"

using v1::crud::Clock;
using v1::crud::decodeToken;
using v1::crud::encodeToken;
using v1::crud::microsToTimePoint;
using v1::crud::timePointToMicros;

UTEST(WebshotCursor, TimePointRoundTrip)
{
    const auto tp = Clock::now();
    const auto micros = timePointToMicros(tp);
    const auto tp2 = microsToTimePoint(micros);
    EXPECT_EQ(timePointToMicros(tp2), micros);
}

UTEST(WebshotCursor, EncodeDecodePaginationCursor)
{
    const auto tp = Clock::time_point(std::chrono::microseconds(123456789));
    const auto micros = timePointToMicros(tp);
    const auto id = boost::uuids::random_generator()();

    dto::PaginationCursor cur(micros, id);
    const auto token = encodeToken(cur);

    const auto decoded = decodeToken<dto::PaginationCursor>(token);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->t, micros);
    EXPECT_EQ(decoded->i, id);
}

UTEST(WebshotCursor, DecodeTokenInvalidReturnsNullopt)
{
    const auto decoded = decodeToken<dto::PaginationCursor>("not-a-token");
    EXPECT_FALSE(decoded);
}
