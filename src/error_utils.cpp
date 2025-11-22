#include "error_utils.hpp"
/**
 * @file
 * @brief Helpers to produce JSON error envelopes used by handlers.
 */
#include "schemas/webshot.hpp"

#include <fmt/format.h>
#include <userver/formats/json/value_builder.hpp>

namespace json = userver::formats::json;

namespace v1::errors {

json::Value makeError(std::string_view message)
{
    dto::ErrorEnvelope::Error err{std::string(message)};
    dto::ErrorEnvelope env{err};
    return json::ValueBuilder(env).ExtractValue();
}

json::Value makeParamError(std::string_view fieldName, std::string_view message)
{
    return makeError(fmt::format("{}: {}", message, fieldName));
}

} // namespace v1::errors
