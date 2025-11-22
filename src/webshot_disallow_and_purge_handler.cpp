#include "webshot_disallow_and_purge_handler.hpp"
/**
 * @file
 * @brief Handler that disallows a host and enqueues purge of its captures.
 */
#include "deadline_utils.hpp"
#include "http_utils.hpp"
#include "link.hpp"
#include "webshot_config.hpp"
#include "webshot_crud.hpp"

#include <chrono>
#include <string>

#include <fmt/format.h>

#include <userver/components/component.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

using namespace v1;
namespace engine = userver::engine;

WebshotDisallowAndPurgeHandler::WebshotDisallowAndPurgeHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<WebshotCrud>()),
      config(context.FindComponent<WebshotConfig>()),
      requestTimeoutMs(config["request-timeout-ms"].As<int64_t>())
{
}

us::yaml_config::Schema WebshotDisallowAndPurgeHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: Webshot disallow-and-purge handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /v1/disallow-and-purge handler in milliseconds
)");
}

std::string WebshotDisallowAndPurgeHandler::
    HandleRequestThrow(const server::http::HttpRequest &request, server::request::RequestContext &)
        const
{
    using server::http::HttpStatus::kAccepted;
    using server::http::HttpStatus::kBadRequest;
    using server::http::HttpStatus::kInternalServerError;

    auto &response = request.GetHttpResponse();
    try {
        const auto handlerTimeout = std::chrono::milliseconds(requestTimeoutMs);
        auto finalDeadline = computeHandlerDeadline(request, handlerTimeout);
        engine::current_task::SetDeadline(finalDeadline);
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("Failed to compute handler deadline: {}", e.what());
        return httpu::respondError(response, kInternalServerError, "internal server error");
    }

    const std::string host = request.GetArg("host");
    if (host.empty())
        return httpu::respondError(response, kBadRequest, "missing parameter: host");
    Link link;
    try {
        link = Link::fromUserInput(host, config.queryPartLengthMax());
    } catch (const InvalidLinkException &e) {
        LOG_INFO() << fmt::format("invalid host: {}", e.what());
        return httpu::respondError(response, kBadRequest, "invalid host");
    }
    LOG_INFO() << fmt::format("invoked for: {}", link.host());
    try {
        crud.disallowAndPurgeHost(link.host());
        response.SetStatus(kAccepted);
        return {};
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("failed for {}: {}", link.host(), e.what());
        return httpu::respondError(response, kInternalServerError, "internal server error");
    }
}
