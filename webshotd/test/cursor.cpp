#include <string>
#include <string_view>

#include <userver/crypto/base64.hpp>
#include <userver/utils/boost_uuid4.hpp>

#include <userver/utest/utest.hpp>

#include "cursor.hpp"
#include "schema/public/webshot.hpp"
#include "text.hpp"

namespace ws {
namespace us = userver;
} // namespace ws

using namespace ws;

using ws::crud::Clock;
using ws::crud::DecodeToken;
using ws::crud::EncodeToken;
using ws::crud::MicrosToTimePoint;
using ws::crud::TimePointToMicros;
using namespace text::literals;

namespace {

[[nodiscard]] String EncodeRawToken(std::string_view bytes)
{
    return *String::FromBytes(
        us::crypto::base64::Base64UrlEncode(bytes, us::crypto::base64::Pad::kWithout)
    );
}

} // namespace

UTEST(Cursor, TimePointRoundTrip)
{
    auto tp = Clock::now();
    auto micros = TimePointToMicros(tp);
    auto tp2 = MicrosToTimePoint(micros);
    EXPECT_EQ(TimePointToMicros(tp2), micros);
}

UTEST(Cursor, EncodeDecodePaginationCursor)
{
    auto tp = Clock::time_point(std::chrono::microseconds(123456789));
    auto micros = TimePointToMicros(tp);
    auto id = us::utils::generators::GenerateBoostUuid();

    dto::PaginationCursor cur(micros, id, dto::PaginationCursor::D::kNext);
    auto token = EncodeToken(cur);

    auto decoded = DecodeToken<dto::PaginationCursor>(token);
    ASSERT_TRUE(decoded);
    if (!decoded)
        return;
    EXPECT_EQ(decoded->t, micros);
    EXPECT_EQ(decoded->i, id);
    EXPECT_EQ(decoded->d, dto::PaginationCursor::D::kNext);
}

UTEST(Cursor, DecodeTokenInvalidReturnsNullopt)
{
    auto decoded = DecodeToken<dto::PaginationCursor>("not-a-token"_t);
    EXPECT_FALSE(decoded);
}

UTEST(Cursor, DecodeTokenInvalidJsonReturnsNullopt)
{
    auto decoded = DecodeToken<dto::PaginationCursor>(EncodeRawToken("{"));
    EXPECT_FALSE(decoded);
}

UTEST(Cursor, DecodeTokenInvalidShapeReturnsNullopt)
{
    auto decoded = DecodeToken<dto::PaginationCursor>(EncodeRawToken("{\"t\":123}"));
    EXPECT_FALSE(decoded);
}
