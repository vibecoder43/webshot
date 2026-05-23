/**
 * @file
 * @brief Implementation of helpers for pagination by exact link.
 */

#include "pagination.hpp"

#include "invariant.hpp"
#include "schema/public/webshot.hpp"
#include "text.hpp"
#include "try.hpp"

namespace ws::crud {

namespace {
[[nodiscard]] dto::PaginationCursor::D ToDto(PageDirection direction)
{
    using namespace text::literals;
    using enum dto::PaginationCursor::D;
    switch (direction) {
    case PageDirection::kNext:
        return kNext;
    case PageDirection::kPrevious:
        return kPrevious;
    }
}

[[nodiscard]] PageDirection FromDto(dto::PaginationCursor::D direction)
{
    using namespace text::literals;
    using enum dto::PaginationCursor::D;
    switch (direction) {
    case kNext:
        return PageDirection::kNext;
    case kPrevious:
        return PageDirection::kPrevious;
    }
}
} // namespace

[[nodiscard]] String
EncodeCursor(Clock::time_point created_at, const Uuid &id, PageDirection direction)
{
    auto micros = TimePointToMicros(created_at);
    dto::PaginationCursor cur(micros, id, ToDto(direction));
    return EncodeToken(cur);
}

[[nodiscard]] std::optional<Cursor> DecodeCursor(const String &token)
{
    auto cur = TRY(DecodeToken<dto::PaginationCursor>(token));
    return Cursor{
        .created_at = MicrosToTimePoint(cur.t),
        .id = cur.i,
        .direction = FromDto(cur.d),
    };
}

} // namespace ws::crud
