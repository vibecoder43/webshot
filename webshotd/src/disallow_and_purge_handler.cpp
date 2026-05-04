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

namespace ws {
namespace us = userver;
namespace server = us::server;
} // namespace ws

using namespace ws;
using namespace text::literals;
using namespace std::chrono_literals;

DisallowAndPurgeHandler::DisallowAndPurgeHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud_(context.FindComponent<Crud>()),
      config_(context.FindComponent<Config>()),
      request_timeout(config["request-timeout-ms"].As<int64_t>() * 1ms)
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
    description: Upper bound for /ws/denylist/disallow_and_purge handler in milliseconds
)");
}

std::string DisallowAndPurgeHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    HandlerRequestSupport request_support{crud_, config_};
    request_support.ApplyRequestDeadline(request, request_timeout);

    const auto link = ParseJsonLinkBody(request, config_);
    if (!link)
        return httpu::RespondError(response, kBadRequest, link.Error());

    LOG_INFO() << std::format("invoked for: {}", link->Host());
    auto prefix_key = prefix::MakePrefixKey(*link);

    const auto cooldown = request_support.CheckClientIpCooldown(request);
    if (!cooldown)
        return RespondClientRequestError(response, cooldown.Error());
    if (*cooldown)
        return httpu::RespondClientIpCooldown(response, (*cooldown)->retry_after);

    auto ok = crud_.DisallowAndPurgePrefix(prefix_key);
    if (!ok) {
        LOG_ERROR() << std::format("disallow_and_purge failed for {}", link->Host());
        return httpu::RespondError(response, kInternalServerError, "internal server error"_t);
    }
    response.SetStatus(kAccepted);
    return {};
}
