/**
 * @file
 * @brief Implementation of helpers for pagination by link prefix.
 */

#include "prefix_pagination.hpp"

#include "integers.hpp"
#include "schema/webshot.hpp"
#include "text.hpp"

#include <userver/utils/assert.hpp>

namespace v1::crud {

[[nodiscard]] std::optional<PrefixCursor> decodePrefixCursor(const String &token)
{
    const auto dtoOpt = decodeToken<dto::PaginationPrefixCursor>(token);
    if (!dtoOpt)
        return {};
    const auto &cur = dtoOpt.value();
    const auto prefix = String::fromBytes(cur.p);
    if (!prefix)
        return {};
    const auto link = String::fromBytes(cur.l);
    if (!link)
        return {};
    PrefixCursor out;
    out.prefix = prefix.value();
    out.link = link.value();
    if (cur.t && cur.i) {
        out.createdAt = microsToTimePoint(cur.t.value());
        out.id = cur.i.value();
    }
    return out;
}

[[nodiscard]] String encodePrefixCursor(const String &prefix, const String &link)
{
    dto::PaginationPrefixCursor cur(std::string(prefix.view()), std::string(link.view()));
    return encodeToken(cur);
}

[[nodiscard]] String encodePrefixCursor(
    const String &prefix, const String &link, Clock::time_point createdAt, const Uuid &id
)
{
    const auto micros = timePointToMicros(createdAt);
    dto::PaginationPrefixCursor cur(
        std::string(prefix.view()), std::string(link.view()), micros, id
    );
    return encodeToken(cur);
}

[[nodiscard]] std::string upperExclusiveBound(String s)
{
    UINVARIANT(!s.empty(), "cannot be empty");
    auto view = s.view();
    std::string bytes(view);
    for (i64 i = safeSize(bytes) - 1_i64; i >= 0_i64; i--) {
        const auto j = numericCast<size_t>(i);
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
