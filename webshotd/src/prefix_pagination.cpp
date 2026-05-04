/**
 * @file
 * @brief Implementation of helpers for pagination by link prefix.
 */

#include "prefix_pagination.hpp"

#include "integers.hpp"
#include "invariant.hpp"
#include "schema/public/webshot.hpp"
#include "text.hpp"
#include "try.hpp"

#include <userver/utils/assert.hpp>

namespace v1::crud {

using namespace text::literals;
using text::ToBytes;

namespace {
[[nodiscard]] dto::PaginationPrefixCursor::D ToDto(PageDirection direction)
{
    using enum PageDirection;
    switch (direction) {
    case kNext:
        return dto::PaginationPrefixCursor::D::kNext;
    case kPrevious:
        return dto::PaginationPrefixCursor::D::kPrevious;
    }
}

[[nodiscard]] PageDirection FromDto(dto::PaginationPrefixCursor::D direction)
{
    using enum dto::PaginationPrefixCursor::D;
    switch (direction) {
    case kNext:
        return PageDirection::kNext;
    case kPrevious:
        return PageDirection::kPrevious;
    }
}
} // namespace

[[nodiscard]] std::optional<PrefixCursor> DecodePrefixCursor(const String &token)
{
    const auto cur = TRY(DecodeToken<dto::PaginationPrefixCursor>(token));
    PrefixCursor out{};
    out.prefix = TRY(String::FromBytes(cur.p));
    out.link = TRY(String::FromBytes(cur.l));
    out.direction = FromDto(cur.d);
    if (cur.t && cur.i) {
        out.created_at = MicrosToTimePoint(*cur.t);
        out.id = *cur.i;
    }
    return out;
}

[[nodiscard]] String
EncodePrefixCursor(const String &prefix, const String &link, PageDirection direction)
{
    dto::PaginationPrefixCursor cur(ToBytes(prefix), ToBytes(link), ToDto(direction));
    return EncodeToken(cur);
}

[[nodiscard]] String EncodePrefixCursor(
    const String &prefix, const String &link, Clock::time_point created_at, const Uuid &id,
    PageDirection direction
)
{
    const auto micros = TimePointToMicros(created_at);
    dto::PaginationPrefixCursor cur(ToBytes(prefix), ToBytes(link), ToDto(direction), micros, id);
    return EncodeToken(cur);
}

[[nodiscard]] std::string UpperExclusiveBound(String s)
{
    auto view = s.View();
    std::string bytes(view);
    for (i64 i = ssize(bytes) - 1_i64; i >= 0_i64; i--) {
        const auto j = NumericCast<size_t>(i);
        unsigned char c = static_cast<unsigned char>(bytes[j]);
        if (c < 0xFF) {
            bytes[j] = static_cast<char>(c + 1);
            bytes.resize(j + 1);
            break;
        }
    }
    return bytes;
}

} // namespace v1::crud
