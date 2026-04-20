#pragma once
/**
 * @file
 * @brief Helpers for producing JSON HTTP responses.
 */
#include "error_utils.hpp"
#include "text.hpp"
#include "userver_namespaces.hpp"

#include <chrono>
#include <format>

#include <userver/formats/json.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/http/content_type.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>

namespace v1::httpu {

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
respondJson(server::http::HttpResponse &resp, server::http::HttpStatus status, const T &body)
{
    resp.SetStatus(status);
    resp.SetContentType(us::http::content_type::kApplicationJson);
    return json::ToString(json::ValueBuilder(body).ExtractValue());
}

/**
 * @brief Variant that takes a prebuilt JSON value.
 */
[[nodiscard]] inline std::string
respondJson(server::http::HttpResponse &resp, server::http::HttpStatus status, json::Value body)
{
    resp.SetStatus(status);
    resp.SetContentType(us::http::content_type::kApplicationJson);
    return json::ToString(std::move(body));
}

/**
 * @brief Write a JSON error envelope with a human-readable message.
 */
[[nodiscard]] inline std::string
respondError(server::http::HttpResponse &resp, server::http::HttpStatus status, String message)
{
    return respondJson(resp, status, v1::errors::makeError(message));
}

[[nodiscard]] inline std::string respondParamError(
    server::http::HttpResponse &resp, server::http::HttpStatus status, String paramName,
    String message
)
{
    return respondJson(resp, status, v1::errors::makeParamError(paramName, message));
}

[[nodiscard]] inline std::string
respondClientIpCooldown(server::http::HttpResponse &resp, std::chrono::milliseconds retryAfter)
{
    using namespace text::literals;

    const auto retryAfterSeconds = std::chrono::ceil<std::chrono::seconds>(retryAfter);
    resp.SetHeader(us::http::headers::kRetryAfter, std::to_string(retryAfterSeconds.count()));
    return respondError(
        resp, server::http::HttpStatus::kTooManyRequests, "client IP in cooldown"_t
    );
}
} // namespace v1::httpu
