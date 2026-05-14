#pragma once
/**
 * @file
 * @brief HTTP helpers: JSON responses and request deadlines for handlers.
 */

#include "deadline_utils.hpp"
#include "error_utils.hpp"
#include "text.hpp"

#include <chrono>
#include <format>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/formats/json.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/http/content_type.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/schema.hpp>

namespace ws {

namespace us = userver;
namespace server = us::server;
namespace eng = us::engine;

// Handler base that enforces per-handler request deadlines.
class [[nodiscard]] DeadlinedHttpHandler : public server::handlers::HttpHandlerBase {
public:
    explicit DeadlinedHttpHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    )
        : HttpHandlerBase(config, context),
          request_timeout_(std::chrono::milliseconds{config["request_timeout_ms"].As<int64_t>()})
    {
    }

    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema()
    {
        return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: Handler static config
additionalProperties: false
properties:
  request_timeout_ms:
    type: integer
    minimum: 1
    description: Upper bound for the handler in milliseconds
)");
    }

    [[nodiscard]]
    std::string HandleRequestThrow(
        const server::http::HttpRequest &request, server::request::RequestContext &context
    ) const final
    {
        auto final_deadline = ComputeHandlerDeadline(request, request_timeout_);
        eng::current_task::SetDeadline(final_deadline);
        return HandleRequestThrowDeadlined(request, context);
    }

protected:
    [[nodiscard]]
    virtual std::string HandleRequestThrowDeadlined(
        const server::http::HttpRequest &request, server::request::RequestContext &context
    ) const = 0;

private:
    const std::chrono::milliseconds request_timeout_;
};

namespace httpu {

namespace json = us::formats::json;

template <typename T>
[[nodiscard]] inline std::string
RespondJson(server::http::HttpResponse &resp, server::http::HttpStatus status, const T &body)
{
    resp.SetStatus(status);
    resp.SetContentType(us::http::content_type::kApplicationJson);
    return json::ToString(json::ValueBuilder(body).ExtractValue());
}

[[nodiscard]] inline std::string
RespondJson(server::http::HttpResponse &resp, server::http::HttpStatus status, json::Value body)
{
    resp.SetStatus(status);
    resp.SetContentType(us::http::content_type::kApplicationJson);
    return json::ToString(std::move(body));
}

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

} // namespace httpu

} // namespace ws
