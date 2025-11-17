#pragma once

/**
 * @file
 * @brief Helpers for pagination by normalized link prefix.
 *
 * Provides a richer cursor type used to page through captures grouped by
 * normalized link prefixes, as well as string range helpers for prefix scans.
 */

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
    std::string prefix;
    std::string link;
    std::optional<Clock::time_point> createdAt;
    std::optional<Uuid> id;
};

/**
 * @brief Decode an opaque token into a prefix-based cursor.
 *
 * @param token Serialized page_token from a previous prefix response.
 * @return Parsed cursor, or empty optional if the token is invalid.
 */
[[nodiscard]] std::optional<PrefixCursor> decodePrefixCursor(const std::string &token);

/**
 * @brief Encode a prefix-based cursor without time/id into an opaque token.
 *
 * Used when the next page starts with the first capture of the next link.
 */
[[nodiscard]] std::string encodePrefixCursor(const std::string &prefix, const std::string &link);

/**
 * @brief Encode a prefix-based cursor with time/id into an opaque token.
 *
 * Used when the next page resumes within the same link after the last item.
 */
[[nodiscard]] std::string encodePrefixCursor(
    const std::string &prefix, const std::string &link, Clock::time_point createdAt, const Uuid &id
);

/**
 * @brief Compute the smallest string strictly greater than any with the same prefix.
 *
 * Treats the input as a byte sequence and increments the last byte that is not
 * 0xFF, truncating the remainder. Returns empty optional if no such bound
 * exists (all bytes are 0xFF).
 */
[[nodiscard]] std::optional<std::string> upperExclusiveBound(std::string s);

} // namespace v1::crud
