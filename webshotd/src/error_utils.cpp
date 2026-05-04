#include "error_utils.hpp"
/**
 * @file
 * @brief Helpers to produce JSON error envelopes used by handlers.
 */
#include "schema/common/common.hpp"
#include "text.hpp"

#include <userver/formats/json/value_builder.hpp>
#include <userver/utils/assert.hpp>

namespace ws::errors {

namespace us = userver;
namespace json = us::formats::json;
using text::ToBytes;

json::Value MakeError(String message)
{
    dto::ErrorEnvelope::Error err(ToBytes(message));
    dto::ErrorEnvelope env(err);
    return json::ValueBuilder(env).ExtractValue();
}

json::Value MakeParamError(String field_name, String message)
{
    return MakeError(text::Format("{}: {}", field_name, message));
}

} // namespace ws::errors
