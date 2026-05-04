#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid.hpp>

namespace ws::uuid {

using Uuid = boost::uuids::uuid;

[[nodiscard]] inline std::optional<Uuid> Parse(std::string_view text) noexcept
{
    boost::uuids::string_generator gen;
    try {
        return gen(std::string{text});
    } catch (const std::runtime_error &) {
        return {};
    }
}

} // namespace ws::uuid
