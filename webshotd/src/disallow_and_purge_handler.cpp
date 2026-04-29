#include "disallow_and_purge_handler.hpp"
/**
 * @file
 * @brief Handler that disallows a host and enqueues purge of its captures.
 */
#include "config.hpp"
#include "crud.hpp"
#include "handler_request_support.hpp"
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
    HandlerRequestSupport requestSupport{crud, config};
    requestSupport.applyRequestDeadline(request, requestTimeout);

    const auto link = parseJsonLinkBody(request, config);
    if (!link)
        return httpu::respondError(response, kBadRequest, link.error());

    LOG_INFO() << std::format("invoked for: {}", link->host());
    auto prefixKey = prefix::makePrefixKey(*link);

    const auto cooldown = requestSupport.checkClientIpCooldown(request);
    if (!cooldown)
        return respondClientRequestError(response, cooldown.error());
    if (*cooldown)
        return httpu::respondClientIpCooldown(response, (*cooldown)->retryAfter);

    auto ok = crud.disallowAndPurgePrefix(prefixKey);
    if (!ok) {
        LOG_ERROR() << std::format("disallow_and_purge failed for {}", link->host());
        return httpu::respondError(response, kInternalServerError, "internal server error"_t);
    }
    response.SetStatus(kAccepted);
    return {};
}
