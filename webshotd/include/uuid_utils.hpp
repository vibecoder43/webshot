#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid.hpp>

namespace v1::uuidu {

using Uuid = boost::uuids::uuid;

[[nodiscard]] inline std::optional<Uuid> parse(std::string_view text) noexcept
{
    boost::uuids::string_generator gen;
    try {
        return gen(std::string{text});
    } catch (const std::runtime_error &) {
        return {};
    }
}

} // namespace v1::uuidu
