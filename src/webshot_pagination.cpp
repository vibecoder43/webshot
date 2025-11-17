/**
 * @file
 * @brief Implementation of helpers for pagination by exact link.
 */

#include "include/webshot_pagination.hpp"

#include "schemas/webshot.hpp"

namespace v1::crud {

[[nodiscard]] std::string encodeCursor(const Cursor &cursor)
{
    const auto micros = timePointToMicros(cursor.createdAt);
    dto::PaginationCursor cur(micros, cursor.id);
    return encodeToken(cur);
}

[[nodiscard]] std::optional<Cursor> decodeCursor(const std::string &token)
{
    const auto dtoOpt = decodeToken<dto::PaginationCursor>(token);
    if (!dtoOpt)
        return std::nullopt;
    const auto &cur = *dtoOpt;
    Cursor out;
    out.createdAt = microsToTimePoint(cur.t);
    out.id = cur.i;
    return out;
}

} // namespace v1::crud
