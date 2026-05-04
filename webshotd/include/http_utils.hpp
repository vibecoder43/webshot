#pragma once
/**
 * @file
 * @brief Helpers for producing JSON HTTP responses.
 */
#include "error_utils.hpp"
#include "text.hpp"

#include <chrono>
#include <format>

#include <userver/formats/json.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/http/content_type.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>

namespace ws::httpu {

namespace us = userver;
namespace server = us::server;
namespace json = us::formats::json;
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
RespondJson(server::http::HttpResponse &resp, server::http::HttpStatus status, const T &body)
{
    resp.SetStatus(status);
    resp.SetContentType(us::http::content_type::kApplicationJson);
    return json::ToString(json::ValueBuilder(body).ExtractValue());
}

/**
 * @brief Variant that takes a prebuilt JSON value.
 */
[[nodiscard]] inline std::string
RespondJson(server::http::HttpResponse &resp, server::http::HttpStatus status, json::Value body)
{
    resp.SetStatus(status);
    resp.SetContentType(us::http::content_type::kApplicationJson);
    return json::ToString(std::move(body));
}

/**
 * @brief Write a JSON error envelope with a human-readable message.
 */
[[nodiscard]] inline std::string
RespondError(server::http::HttpResponse &resp, server::http::HttpStatus status, String message)
{
    return RespondJson(resp, status, ws::errors::MakeError(message));
}

[[nodiscard]] inline std::string RespondParamError(
    server::http::HttpResponse &resp, server::http::HttpStatus status, String param_name,
    String message
)
{
    return RespondJson(resp, status, ws::errors::MakeParamError(param_name, message));
}

[[nodiscard]] inline std::string
RespondClientIpCooldown(server::http::HttpResponse &resp, std::chrono::milliseconds retry_after)
{
    using namespace text::literals;

    const auto retry_after_seconds = std::chrono::ceil<std::chrono::seconds>(retry_after);
    resp.SetHeader(us::http::headers::kRetryAfter, std::to_string(retry_after_seconds.count()));
    return RespondError(
        resp, server::http::HttpStatus::kTooManyRequests, "client IP in cooldown"_t
    );
}
} // namespace ws::httpu
