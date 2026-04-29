#include "allowlist_handler.hpp"
/**
 * @file
 * @brief Internal endpoints for checking and editing the allowlist.
 */
#include "config.hpp"
#include "deadline_utils.hpp"
#include "denylist.hpp"
#include "handler_request_support.hpp"
#include "http_utils.hpp"
#include "integers.hpp"
#include "link.hpp"
#include "metrics.hpp"
#include "prefix_utils.hpp"
#include "text.hpp"
#include "try.hpp"

#include <chrono>
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

namespace v1 {
using namespace std::chrono_literals;
using namespace text::literals;

namespace {

void applyDeadline(
    const server::http::HttpRequest &request, std::chrono::milliseconds requestTimeout
)
{
    auto finalDeadline = computeHandlerDeadline(request, requestTimeout);
    eng::current_task::SetDeadline(finalDeadline);
}

} // namespace

AllowlistCheckHandler::AllowlistCheckHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), config(context.FindComponent<Config>()),
      denylist(context.FindComponent<Denylist>()), metrics(context.FindComponent<Metrics>()),
      requestTimeout(config["request-timeout-ms"].As<int64_t>() * 1ms)
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
    description: Upper bound for /v1/allowlist/check handler in milliseconds
)");
}

std::string AllowlistCheckHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using server::http::HttpMethod::kPost;
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    applyDeadline(request, requestTimeout);

    if (request.GetMethod() != kPost) {
        response.SetStatus(kMethodNotAllowed);
        return {};
    }

    const auto link = parseJsonLinkBody(request, config);
    if (!link)
        return httpu::respondError(response, kBadRequest, link.error());

    const auto allowed = denylist.isAllowlistedPrefix(prefix::makePrefixKey(*link));
    if (!allowed) {
        metrics.accountError(Metrics::Error::kDenylistCheck);
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
    : HttpHandlerBase(config, context), config(context.FindComponent<Config>()),
      denylist(context.FindComponent<Denylist>()), metrics(context.FindComponent<Metrics>()),
      requestTimeout(config["request-timeout-ms"].As<int64_t>() * 1ms)
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
    description: Upper bound for /v1/allowlist/add handler in milliseconds
)");
}

std::string AllowlistAddHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using server::http::HttpMethod::kPost;
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    applyDeadline(request, requestTimeout);

    if (request.GetMethod() != kPost) {
        response.SetStatus(kMethodNotAllowed);
        return {};
    }

    const auto link = parseJsonLinkBody(request, config);
    if (!link)
        return httpu::respondError(response, kBadRequest, link.error());

    const auto prefixKey = prefix::makePrefixKey(*link);
    const auto inserted = denylist.insertAllowlistPrefix(prefixKey, "allowlist_add"_t);
    if (!inserted) {
        metrics.accountError(Metrics::Error::kDenylistCheck);
        LOG_ERROR() << std::format("allowlist add failed for {}", prefixKey);
        return httpu::respondError(response, kInternalServerError, "internal server error"_t);
    }

    response.SetStatus(kNoContent);
    return {};
}

AllowlistRemoveHandler::AllowlistRemoveHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), config(context.FindComponent<Config>()),
      denylist(context.FindComponent<Denylist>()), metrics(context.FindComponent<Metrics>()),
      requestTimeout(config["request-timeout-ms"].As<int64_t>() * 1ms)
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
    description: Upper bound for /v1/allowlist/remove handler in milliseconds
)");
}

std::string AllowlistRemoveHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using server::http::HttpMethod::kPost;
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    applyDeadline(request, requestTimeout);

    if (request.GetMethod() != kPost) {
        response.SetStatus(kMethodNotAllowed);
        return {};
    }

    const auto link = parseJsonLinkBody(request, config);
    if (!link)
        return httpu::respondError(response, kBadRequest, link.error());

    const auto prefixKey = prefix::makePrefixKey(*link);
    const auto removed = denylist.removeAllowlistPrefix(prefixKey);
    if (!removed) {
        metrics.accountError(Metrics::Error::kDenylistCheck);
        LOG_ERROR() << std::format("allowlist remove failed for {}", prefixKey);
        return httpu::respondError(response, kInternalServerError, "internal server error"_t);
    }

    response.SetStatus(kNoContent);
    return {};
}

} // namespace v1
