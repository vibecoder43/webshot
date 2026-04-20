#include "disallow_and_purge_handler.hpp"
/**
 * @file
 * @brief Handler that disallows a host and enqueues purge of its captures.
 */
#include "client_ip.hpp"
#include "config.hpp"
#include "crud.hpp"
#include "deadline_utils.hpp"
#include "http_utils.hpp"
#include "integers.hpp"
#include "link.hpp"
#include "prefix_utils.hpp"
#include "text.hpp"

#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include <userver/components/component.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/utils/assert.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

using namespace v1;
using namespace text::literals;
using namespace std::chrono_literals;

DisallowAndPurgeHandler::DisallowAndPurgeHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<Crud>()),
      config(context.FindComponent<Config>()),
      requestTimeout(config["request-timeout-ms"].As<int64_t>() * 1ms)
{
}

us::yaml_config::Schema DisallowAndPurgeHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: Disallow_and_purge handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /v1/denylist/disallow_and_purge handler in milliseconds
)");
}

std::string DisallowAndPurgeHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    auto finalDeadline = computeHandlerDeadline(request, requestTimeout);
    eng::current_task::SetDeadline(finalDeadline);

    const std::string arg = request.GetArg("host");
    if (arg.empty())
        return httpu::respondParamError(response, kBadRequest, "host"_t, "missing parameter"_t);
    const auto host = String::fromBytes(arg);
    if (!host)
        return httpu::respondParamError(response, kBadRequest, "host"_t, "invalid parameter"_t);
    const auto link = Link::fromText(*host, config.urlBytesMax());
    if (!link) {
        LOG_INFO() << "invalid host";
        return httpu::respondParamError(response, kBadRequest, "host"_t, "invalid parameter"_t);
    }
    LOG_INFO() << std::format("invoked for: {}", link->host());
    auto prefixKey = prefix::makePrefixKey(*link);
    auto clientIp = client_ip::resolve(request, config);
    if (!clientIp)
        return httpu::respondError(response, kBadRequest, "invalid client ip"_t);
    auto cooldown = *crud.acquireClientIpCooldown(std::move(*clientIp));
    if (cooldown)
        return httpu::respondClientIpCooldown(response, cooldown->retryAfter);

    auto ok = crud.disallowAndPurgePrefix(prefixKey);
    if (!ok) {
        LOG_ERROR() << std::format("disallow_and_purge failed for {}", link->host());
        return httpu::respondError(response, kInternalServerError, "internal server error"_t);
    }
    response.SetStatus(kAccepted);
    return {};
}
