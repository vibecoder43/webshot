#include "allowlist_handler.hpp"
/**
 * @file
 * @brief Internal endpoints for checking and editing the allowlist.
 */
#include "config.hpp"
#include "crud.hpp"
#include "denylist.hpp"
#include "handler_request_support.hpp"
#include "http_utils.hpp"
#include "metrics.hpp"
#include "prefix_utils.hpp"
#include "text.hpp"

#include <format>
#include <string>

#include <userver/components/component.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/http/http_method.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace ws {
namespace us = userver;
namespace server = us::server;
namespace eng = us::engine;
using namespace std::chrono_literals;
using namespace text::literals;

AllowlistCheckHandler::AllowlistCheckHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), config_(context.FindComponent<Config>()),
      denylist_(context.FindComponent<Denylist>()), metrics_(context.FindComponent<Metrics>()),
      crud_(context.FindComponent<Crud>()),
      request_timeout(config["request-timeout-ms"].As<int64_t>() * 1ms)
{
}

us::yaml_config::Schema AllowlistCheckHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: Allowlist check handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /ws/allowlist/check handler in milliseconds
)");
}

std::string AllowlistCheckHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    auto final_deadline = ComputeHandlerDeadline(request, request_timeout);
    eng::current_task::SetDeadline(final_deadline);

    const auto link = ParseJsonLinkBody(request, config_);
    if (!link)
        return httpu::RespondError(response, kBadRequest, link.Error());

    const auto allowed = denylist_.IsAllowlistedPrefix(prefix::MakePrefixKey(*link));
    if (!allowed) {
        metrics_.AccountError(Metrics::Error::kDenylistCheck);
        response.SetStatus(kInternalServerError);
        return {};
    }
    if (!*allowed) {
        response.SetStatus(kForbidden);
        return {};
    }

    response.SetStatus(kNoContent);
    return {};
}

AllowlistAddHandler::AllowlistAddHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), config_(context.FindComponent<Config>()),
      denylist_(context.FindComponent<Denylist>()), metrics_(context.FindComponent<Metrics>()),
      crud_(context.FindComponent<Crud>()),
      request_timeout(config["request-timeout-ms"].As<int64_t>() * 1ms)
{
}

us::yaml_config::Schema AllowlistAddHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: Allowlist add handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /ws/allowlist/add handler in milliseconds
)");
}

std::string AllowlistAddHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    auto final_deadline = ComputeHandlerDeadline(request, request_timeout);
    eng::current_task::SetDeadline(final_deadline);

    const auto link = ParseJsonLinkBody(request, config_);
    if (!link)
        return httpu::RespondError(response, kBadRequest, link.Error());

    const auto prefix_key = prefix::MakePrefixKey(*link);
    const auto inserted = denylist_.InsertAllowlistPrefix(prefix_key, "allowlist_add"_t);
    if (!inserted) {
        metrics_.AccountError(Metrics::Error::kDenylistCheck);
        LOG_ERROR() << std::format("allowlist add failed for {}", prefix_key);
        return httpu::RespondError(response, kInternalServerError, "internal server error"_t);
    }

    response.SetStatus(kNoContent);
    return {};
}

AllowlistRemoveHandler::AllowlistRemoveHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), config_(context.FindComponent<Config>()),
      denylist_(context.FindComponent<Denylist>()), metrics_(context.FindComponent<Metrics>()),
      crud_(context.FindComponent<Crud>()),
      request_timeout(config["request-timeout-ms"].As<int64_t>() * 1ms)
{
}

us::yaml_config::Schema AllowlistRemoveHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: Allowlist remove handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /ws/allowlist/remove handler in milliseconds
)");
}

std::string AllowlistRemoveHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    const auto link = ParseJsonLinkBody(request, config_);
    if (!link)
        return httpu::RespondError(response, kBadRequest, link.Error());

    const auto prefix_key = prefix::MakePrefixKey(*link);
    const auto removed = denylist_.RemoveAllowlistPrefix(prefix_key);
    if (!removed) {
        metrics_.AccountError(Metrics::Error::kDenylistCheck);
        LOG_ERROR() << std::format("allowlist remove failed for {}", prefix_key);
        return httpu::RespondError(response, kInternalServerError, "internal server error"_t);
    }
    response.SetStatus(kNoContent);
    return {};
}

} // namespace ws
