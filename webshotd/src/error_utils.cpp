#include "error_utils.hpp"
/**
 * @file
 * @brief Helpers to produce JSON error envelopes used by handlers.
 */
#include "schema/webshot.hpp"
#include "text.hpp"
#include "userver_namespaces.hpp"

#include <userver/formats/json/value_builder.hpp>
#include <userver/utils/assert.hpp>

namespace v1::errors {

json::Value makeError(String message)
{
    dto::ErrorEnvelope::Error err(std::to_string(message));
    dto::ErrorEnvelope env(err);
    return json::ValueBuilder(env).ExtractValue();
}

json::Value makeParamError(String fieldName, String message)
{
    return makeError(text::format("{}: {}", fieldName, message));
}

} // namespace v1::errors
