#pragma once

#include <string>
#include <string_view>

#include <userver/formats/json.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/http/content_type.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>

#include "error_utils.hpp"

namespace v1::httpu {
namespace json = userver::formats::json;
namespace http = userver::server::http;

template <typename T>
[[nodiscard]] inline std::string
respondJson(http::HttpResponse &resp, http::HttpStatus status, const T &body)
{
    resp.SetStatus(status);
    resp.SetContentType(userver::http::content_type::kApplicationJson);
    return json::ToString(json::ValueBuilder(body).ExtractValue());
}

[[nodiscard]] inline std::string
respondJson(http::HttpResponse &resp, http::HttpStatus status, json::Value body)
{
    resp.SetStatus(status);
    resp.SetContentType(userver::http::content_type::kApplicationJson);
    return json::ToString(std::move(body));
}

[[nodiscard]] inline std::string
respondError(http::HttpResponse &resp, http::HttpStatus status, std::string_view message)
{
    return respondJson(resp, status, v1::errors::makeError(message));
}

// Removed field-level variant; build a message and use respondError instead.

// Text variant intentionally omitted to keep API JSON-only for responses.
} // namespace v1::httpu
