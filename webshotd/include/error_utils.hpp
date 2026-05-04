#pragma once

#include "text.hpp"

#include <userver/formats/json/value.hpp>

namespace ws::errors {

namespace us = userver;
namespace json = us::formats::json;
/**
 * @brief Build a standard JSON error envelope with a message.
 *
 * The shape matches the OpenAPI schema under `ErrorEnvelope`.
 */
json::Value MakeError(String message);

/**
 * @brief Convenience for parameter-specific errors.
 *
 * @param fieldName Name of the offending parameter.
 * @param message Additional context to append to the message.
 */
json::Value MakeParamError(String field_name, String message);

} // namespace ws::errors
