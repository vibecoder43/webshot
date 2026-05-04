#pragma once

#include "uuid_utils.hpp"

#include <boost/uuid/uuid_io.hpp>

#include <format>
#include <string>
#include <string_view>

namespace ws::uuid {

[[nodiscard]] inline std::string ToBytes(const Uuid &value)
{
    return boost::uuids::to_string(value);
}

} // namespace ws::uuid

template <> struct std::formatter<ws::uuid::Uuid, char> : std::formatter<std::string_view, char> {
    auto format(const ws::uuid::Uuid &value, std::format_context &ctx) const
    {
        const auto text = boost::uuids::to_string(value);
        return std::formatter<std::string_view, char>::format(text, ctx);
    }
};
