#pragma once
/**
 * @file
 * @brief Helpers for producing JSON HTTP responses.
 */
#include "error_utils.hpp"
#include "text.hpp"

#include <userver/formats/json.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/http/content_type.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>

namespace v1::httpu {
namespace json = userver::formats::json;
namespace http = userver::server::http;

/**
 * @brief Serialize an object to JSON and set status and content type.
 *
 * @tparam T Type supported by userver JSON serialization.
 * @param resp Response to write headers into.
 * @param status HTTP status to set.
 * @param body JSON-serializable value.
 * @return Response body as a JSON string.
 */
template <typename T>
[[nodiscard]] inline std::string
respondJson(http::HttpResponse &resp, http::HttpStatus status, const T &body)
{
    resp.SetStatus(status);
    resp.SetContentType(userver::http::content_type::kApplicationJson);
    return json::ToString(json::ValueBuilder(body).ExtractValue());
}

/**
 * @brief Variant that takes a prebuilt JSON value.
 */
[[nodiscard]] inline std::string
respondJson(http::HttpResponse &resp, http::HttpStatus status, json::Value body)
{
    resp.SetStatus(status);
    resp.SetContentType(userver::http::content_type::kApplicationJson);
    return json::ToString(std::move(body));
}

/**
 * @brief Write a JSON error envelope with a human-readable message.
 */
[[nodiscard]] inline std::string
respondError(http::HttpResponse &resp, http::HttpStatus status, String message)
{
    return respondJson(resp, status, v1::errors::makeError(message));
}

[[nodiscard]] inline std::string respondParamError(
    http::HttpResponse &resp, http::HttpStatus status, String paramName, String message
)
{
    return respondJson(resp, status, v1::errors::makeParamError(paramName, message));
}
} // namespace v1::httpu
