#pragma once

#include <string_view>

#include <userver/formats/json/value.hpp>

namespace v1::errors {

/**
 * @brief Build a standard JSON error envelope with a message.
 *
 * The shape matches the OpenAPI schema under `ErrorEnvelope`.
 */
userver::formats::json::Value makeError(std::string_view message);

/**
 * @brief Convenience for parameter‑specific errors.
 *
 * @param param_name Name of the offending parameter.
 * @param detail Additional context to append to the message.
 */
userver::formats::json::Value makeParamError(std::string_view param_name, std::string_view detail);

} // namespace v1::errors
