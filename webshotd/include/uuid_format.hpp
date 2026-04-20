#pragma once

#include "uuid_utils.hpp"

#include <boost/uuid/uuid_io.hpp>

#include <format>
#include <string>
#include <string_view>

namespace std {

using v1::uuidu::Uuid;

template <> struct formatter<Uuid, char> : formatter<std::string_view, char> {
    auto format(const Uuid &value, format_context &ctx) const
    {
        const auto text = boost::uuids::to_string(value);
        return formatter<std::string_view, char>::format(text, ctx);
    }
};

[[nodiscard]] inline std::string to_string(const Uuid &value)
{
    return boost::uuids::to_string(value);
}

} // namespace std
