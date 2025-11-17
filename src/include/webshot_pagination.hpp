#pragma once

/**
 * @file
 * @brief Helpers for pagination by exact normalized link.
 *
 * Defines a lightweight pagination cursor and helpers to serialize/deserialize
 * it as an opaque token suitable for use in API page_token fields.
 */

#include "webshot_cursor.hpp"

#include <optional>
#include <string>

#include <boost/uuid/uuid.hpp>

namespace v1::crud {

using Uuid = boost::uuids::uuid;

/**
 * @brief Cursor that identifies the last item in a link-based page.
 *
 * Holds the creation time and UUID of the last capture returned to the client
 * so that the next page can resume after that item.
 */
struct [[nodiscard]] Cursor {
    Clock::time_point createdAt;
    Uuid id;
};

/**
 * @brief Encode a link-based cursor into an opaque token.
 *
 * The resulting string is suitable for direct use as a page_token.
 */
[[nodiscard]] std::string encodeCursor(const Cursor &cursor);

/**
 * @brief Decode an opaque token into a link-based cursor.
 *
 * @param token Serialized page_token from a previous response.
 * @return Parsed cursor, or empty optional if the token is invalid.
 */
[[nodiscard]] std::optional<Cursor> decodeCursor(const std::string &token);

} // namespace v1::crud
