#pragma once

/**
 * @file
 * @brief Helpers for pagination by normalized link prefix.
 *
 * Provides a richer cursor type used to page through captures grouped by
 * normalized link prefixes, as well as string range helpers for prefix scans.
 */

#include "text.hpp"
#include "webshot_cursor.hpp"

#include <optional>
#include <string>

#include <boost/uuid/uuid.hpp>

namespace v1::crud {

using Uuid = boost::uuids::uuid;

/**
 * @brief Cursor that identifies position in a prefix-based page.
 *
 * Contains the prefix, the last link seen for that prefix, and optionally the
 * time and UUID of the last capture when resuming in the middle of a link.
 */
struct [[nodiscard]] PrefixCursor {
    String prefix;
    String link;
    std::optional<Clock::time_point> createdAt;
    std::optional<Uuid> id;
};

/**
 * @brief Decode an opaque token into a prefix-based cursor.
 *
 * @param token Serialized page_token from a previous prefix response.
 * @return Parsed cursor, or empty optional if the token is invalid.
 */
[[nodiscard]] std::optional<PrefixCursor> decodePrefixCursor(const String &token);

/**
 * @brief Encode a prefix-based cursor without time/id into an opaque token.
 *
 * Used when the next page starts with the first capture of the next link.
 */
[[nodiscard]] String encodePrefixCursor(const String &prefix, const String &link);

/**
 * @brief Encode a prefix-based cursor with time/id into an opaque token.
 *
 * Used when the next page resumes within the same link after the last item.
 */
[[nodiscard]] String encodePrefixCursor(
    const String &prefix, const String &link, Clock::time_point createdAt, const Uuid &id
);

/**
 * @brief Compute the smallest string strictly greater than any with the same prefix.
 *
 * Treats the input as a byte sequence and increments the last byte that is not
 * 0xFF, truncating the remainder. Requires a non-empty input string.
 */
[[nodiscard]] std::string upperExclusiveBound(String s);

} // namespace v1::crud
