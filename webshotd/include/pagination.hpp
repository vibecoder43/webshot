#pragma once

/**
 * @file
 * @brief Helpers for pagination by exact normalized link.
 *
 * Defines a lightweight pagination cursor and helpers to serialize/deserialize
 * it as an opaque token suitable for use in API page_token fields.
 */
#include "cursor.hpp"
#include "text.hpp"

#include <optional>

#include <boost/uuid/uuid.hpp>

namespace ws::crud {

using Uuid = boost::uuids::uuid;

/**
 * @brief Cursor that identifies the last item in a link-based page.
 *
 * Holds the creation time, UUID, and direction of the capture used as a
 * cursor boundary.
 */
struct [[nodiscard]] Cursor {
    Clock::time_point created_at;
    Uuid id;
    PageDirection direction;
};

/**
 * @brief Encode a link-based cursor into an opaque token.
 *
 * The resulting string is suitable for direct use as a page_token.
 */
[[nodiscard]] String
EncodeCursor(Clock::time_point created_at, const Uuid &id, PageDirection direction);

/**
 * @brief Decode an opaque token into a link-based cursor.
 *
 * @param token Serialized page_token from a previous response.
 * @return Parsed cursor, or empty optional if the token is invalid.
 */
[[nodiscard]] std::optional<Cursor> DecodeCursor(const String &token);

} // namespace ws::crud
