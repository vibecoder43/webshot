#pragma once

/**
 * @file
 * @brief Helpers for pagination by normalized link prefix.
 *
 * Provides a richer cursor type used to page through captures grouped by
 * normalized link prefixes, as well as string range helpers for prefix scans.
 */

#include "cursor.hpp"
#include "text.hpp"

#include <optional>
#include <string>

#include <boost/uuid/uuid.hpp>

namespace ws::crud {

using Uuid = boost::uuids::uuid;

/**
 * @brief Cursor that identifies position in a prefix-based page.
 *
 * Contains the prefix, the boundary link, page direction, and optionally the
 * time and UUID of a capture when resuming in the middle of a link.
 */
struct [[nodiscard]] PrefixCursor {
    String prefix;
    String link;
    PageDirection direction;
    std::optional<Clock::time_point> created_at;
    std::optional<Uuid> id;
};

/**
 * @brief Decode an opaque token into a prefix-based cursor.
 *
 * @param token Serialized page_token from a previous prefix response.
 * @return Parsed cursor, or empty optional if the token is invalid.
 */
[[nodiscard]] std::optional<PrefixCursor> DecodePrefixCursor(const String &token);

/**
 * @brief Encode a prefix-based cursor without time/id into an opaque token.
 *
 * Used when pagination starts at an adjacent link.
 */
[[nodiscard]] String
EncodePrefixCursor(const String &prefix, const String &link, PageDirection direction);

/**
 * @brief Encode a prefix-based cursor with time/id into an opaque token.
 *
 * Used when pagination resumes within the same link.
 */
[[nodiscard]] String EncodePrefixCursor(
    const String &prefix, const String &link, Clock::time_point created_at, const Uuid &id,
    PageDirection direction
);

/**
 * @brief Compute the smallest string strictly greater than any with the same prefix.
 *
 * Treats the input as a byte sequence and increments the last byte that is not
 * 0xFF, truncating the remainder. Requires a non-empty input string.
 */
[[nodiscard]] std::string UpperExclusiveBound(String s);

} // namespace ws::crud
