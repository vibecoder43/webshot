#include <string>

#include <userver/utils/boost_uuid4.hpp>

#include <userver/utest/utest.hpp>

#include "pagination.hpp"
#include "text.hpp"

namespace ws {
namespace us = userver;
} // namespace ws

using namespace ws;

using ws::crud::Clock;
using ws::crud::Cursor;
using ws::crud::DecodeCursor;
using ws::crud::EncodeCursor;
using ws::crud::PageDirection;
using ws::crud::TimePointToMicros;
using namespace text::literals;

UTEST(Pagination, CursorRoundTrip)
{
    Cursor cursor{
        Clock::time_point(std::chrono::microseconds(987654321)),
        us::utils::generators::GenerateBoostUuid(),
        PageDirection::kPrevious,
    };

    auto token = EncodeCursor(cursor.created_at, cursor.id, cursor.direction);
    auto decoded = DecodeCursor(token);
    ASSERT_TRUE(decoded);
    if (!decoded)
        return;
    EXPECT_EQ(TimePointToMicros(decoded->created_at), TimePointToMicros(cursor.created_at));
    EXPECT_EQ(decoded->id, cursor.id);
    EXPECT_EQ(decoded->direction, cursor.direction);
}

UTEST(Pagination, DecodeCursorInvalidReturnsNullopt)
{
    auto decoded = DecodeCursor("invalid-token"_t);
    EXPECT_FALSE(decoded);
}
