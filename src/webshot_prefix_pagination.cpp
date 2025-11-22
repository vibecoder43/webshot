/**
 * @file
 * @brief Implementation of helpers for pagination by link prefix.
 */

#include "webshot_prefix_pagination.hpp"

#include "schemas/webshot.hpp"

namespace v1::crud {

[[nodiscard]] std::optional<PrefixCursor> decodePrefixCursor(const std::string &token)
{
    const auto dtoOpt = decodeToken<dto::PaginationPrefixCursor>(token);
    if (!dtoOpt)
        return std::nullopt;

    const auto &cur = *dtoOpt;
    PrefixCursor out;
    out.prefix = cur.p;
    out.link = cur.l;
    if (cur.t && cur.i) {
        out.createdAt = microsToTimePoint(*cur.t);
        out.id = *cur.i;
    }
    return out;
}

[[nodiscard]] std::string encodePrefixCursor(const std::string &prefix, const std::string &link)
{
    dto::PaginationPrefixCursor cur(prefix, link);
    return encodeToken(cur);
}

[[nodiscard]] std::string encodePrefixCursor(
    const std::string &prefix, const std::string &link, Clock::time_point createdAt, const Uuid &id
)
{
    const auto micros = timePointToMicros(createdAt);
    dto::PaginationPrefixCursor cur(prefix, link, micros, id);
    return encodeToken(cur);
}

[[nodiscard]] std::optional<std::string> upperExclusiveBound(std::string s)
{
    for (int64_t i = static_cast<int64_t>(s.size()) - 1; i >= 0; i--) {
        unsigned char c = static_cast<unsigned char>(s[static_cast<std::size_t>(i)]);
        if (c < 0xFF) {
            s[static_cast<std::size_t>(i)] = static_cast<char>(c + 1);
            s.resize(static_cast<std::size_t>(i) + 1);
            return s;
        }
    }
    return {};
}

} // namespace v1::crud
