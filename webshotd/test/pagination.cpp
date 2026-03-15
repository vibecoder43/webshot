#include <string>

#include <userver/utils/boost_uuid4.hpp>

#include <userver/utest/utest.hpp>

#include "pagination.hpp"
#include "text.hpp"

using v1::crud::Clock;
using v1::crud::Cursor;
using v1::crud::decodeCursor;
using v1::crud::encodeCursor;
using v1::crud::timePointToMicros;
using namespace text::literals;

UTEST(Pagination, CursorRoundTrip)
{
    Cursor cursor{
        Clock::time_point(std::chrono::microseconds(987654321)),
        userver::utils::generators::GenerateBoostUuid(),
    };

    const auto token = encodeCursor(cursor);
    const auto decoded = decodeCursor(token);
    ASSERT_TRUE(decoded);
    if (!decoded)
        return;
    EXPECT_EQ(timePointToMicros(decoded->createdAt), timePointToMicros(cursor.createdAt));
    EXPECT_EQ(decoded->id, cursor.id);
}

UTEST(Pagination, DecodeCursorInvalidReturnsNullopt)
{
    const auto decoded = decodeCursor("invalid-token"_t);
    EXPECT_FALSE(decoded);
}
