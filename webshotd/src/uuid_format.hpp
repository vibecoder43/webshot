#pragma once

#include "uuid_utils.hpp"

#include <boost/uuid/uuid_io.hpp>

#include <format>
#include <string>
#include <string_view>

namespace v1::uuidu {

[[nodiscard]] inline std::string toBytes(const Uuid &value)
{
    return boost::uuids::to_string(value);
}

} // namespace v1::uuidu

template <> struct std::formatter<v1::uuidu::Uuid, char> : std::formatter<std::string_view, char> {
    auto format(const v1::uuidu::Uuid &value, std::format_context &ctx) const
    {
        const auto text = boost::uuids::to_string(value);
        return std::formatter<std::string_view, char>::format(text, ctx);
    }
};
