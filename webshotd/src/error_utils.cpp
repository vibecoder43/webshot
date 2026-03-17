/**
 * @file
 * @brief Helpers to produce JSON error envelopes used by handlers.
 */

#include "error_utils.hpp"
#include "schema/webshot.hpp"
#include "text.hpp"
#include <string>
#include <userver/formats/json/value_builder.hpp>

namespace json = userver::formats::json;

namespace v1::errors {

json::Value makeError(String message)
{
    dto::ErrorEnvelope::Error err(std::string(message.view()));
    dto::ErrorEnvelope env(err);
    return json::ValueBuilder(env).ExtractValue();
}

json::Value makeParamError(String fieldName, String message)
{
    return makeError(text::format("{}: {}", fieldName, message));
}

} // namespace v1::errors
