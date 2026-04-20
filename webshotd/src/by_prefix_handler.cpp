#include "by_prefix_handler.hpp"
/**
 * @file
 * @brief Handler that lists captures grouped by normalized link prefix.
 */
#include "client_ip.hpp"
#include "config.hpp"
#include "crud.hpp"
#include "deadline_utils.hpp"
#include "integers.hpp"
#include "link.hpp"
#include "text.hpp"

#include <chrono>
#include <format>
#include <string>
#include <utility>

#include "http_utils.hpp"
#include "server_errors.hpp"
#include <userver/components/component.hpp>
#include <userver/engine/exception.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/formats/json.hpp>
#include <userver/http/content_type.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/http/http_method.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

using namespace v1;
using namespace text::literals;
using namespace std::chrono_literals;

ByPrefixHandler::ByPrefixHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<Crud>()),
      cfg(context.FindComponent<Config>()),
      requestTimeout(config["request-timeout-ms"].As<int64_t>() * 1ms)
{
}

us::yaml_config::Schema ByPrefixHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: By_prefix handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /v1/capture/prefix handler in milliseconds
)");
}

std::string ByPrefixHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;
    auto &response = request.GetHttpResponse();
    auto finalDeadline = computeHandlerDeadline(request, requestTimeout);
    eng::current_task::SetDeadline(finalDeadline);

    const std::string arg = request.GetArg("prefix");
    if (arg.empty())
        return httpu::respondParamError(response, kBadRequest, "prefix"_t, "missing parameter"_t);

    auto prefix = String::fromBytes(arg);
    if (!prefix)
        return httpu::respondParamError(response, kBadRequest, "prefix"_t, "invalid parameter"_t);

    const auto parsed = Link::fromText(*prefix, cfg.urlBytesMax());
    if (!parsed)
        return httpu::respondParamError(response, kBadRequest, "prefix"_t, "invalid parameter"_t);

    const auto normalizedPrefix = parsed->normalized();
    const std::string tokenArg = request.GetArg("page_token");
    const auto token = String::fromBytes(tokenArg);
    if (!token)
        return httpu::respondParamError(
            response, kBadRequest, "page_token"_t, "invalid parameter"_t
        );

    auto clientIp = client_ip::resolve(request, cfg);
    if (!clientIp)
        return httpu::respondError(response, kBadRequest, "invalid client ip"_t);
    auto cooldown = *crud.acquireClientIpCooldown(std::move(*clientIp));
    if (cooldown)
        return httpu::respondClientIpCooldown(response, cooldown->retryAfter);

    auto page = crud.findCapturesByPrefixPage(normalizedPrefix, *token);
    if (!page) {
        using enum errors::CapturePageError;
        if (page.error() == kDbFailure)
            return httpu::respondError(response, kInternalServerError, "internal server error"_t);
        return httpu::respondParamError(
            response, kBadRequest, "page_token"_t, "invalid page_token"_t
        );
    }
    return httpu::respondJson(response, kOk, *page);
}
