#include "include/webshots_by_prefix_handler.hpp"
/**
 * @file
 * @brief Handler that lists captures grouped by normalized link prefix.
 */
#include "include/deadline_utils.hpp"
#include "include/link.hpp"
#include "include/webshot_config.hpp"
#include "include/webshot_crud.hpp"

#include <chrono>
#include <string>

#include "include/http_utils.hpp"
#include "include/server_errors.hpp"
#include <userver/components/component.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/formats/json.hpp>
#include <userver/http/content_type.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/http/http_method.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace us = userver;

using namespace v1;
namespace engine = userver::engine;

WebshotsByPrefixHandler::WebshotsByPrefixHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<WebshotCrud>()),
      cfg(context.FindComponent<WebshotConfig>()),
      requestTimeoutMs(config["request-timeout-ms"].As<int64_t>())
{
}

us::yaml_config::Schema WebshotsByPrefixHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: Webshots-by-prefix handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /v1/webshot/prefix handler in milliseconds
)");
}

std::string WebshotsByPrefixHandler::
    HandleRequestThrow(const server::http::HttpRequest &request, server::request::RequestContext &)
        const
{
    using server::http::HttpStatus::kBadRequest;
    using server::http::HttpStatus::kInternalServerError;
    using server::http::HttpStatus::kMethodNotAllowed;
    using server::http::HttpStatus::kOk;
    using us::http::content_type::kApplicationJson;
    auto &response = request.GetHttpResponse();
    try {
        const auto handlerTimeout = std::chrono::milliseconds(requestTimeoutMs);
        auto finalDeadline = computeHandlerDeadline(request, handlerTimeout);
        engine::current_task::SetDeadline(finalDeadline);

        const std::string prefixArg = request.GetArg("prefix");
        if (prefixArg.empty())
            return httpu::respondError(response, kBadRequest, "missing parameter: prefix");
        std::string normalizedPrefix;
        try {
            normalizedPrefix =
                Link::fromUserInput(prefixArg, cfg.queryPartLengthMax()).normalized();
        } catch (const InvalidLinkException &e) {
            return httpu::respondError(response, kBadRequest, e.what());
        }
        const auto token = request.GetArg("page_token");

        try {
            auto page = crud.findWebshotsByPrefixPage(
                normalizedPrefix, token.empty() ? std::nullopt : std::optional<std::string>(token)
            );
            return httpu::respondJson(response, kOk, page);
        } catch (const errors::InvalidPageTokenException &) {
            return httpu::respondError(response, kBadRequest, "invalid page_token");
        }
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("Unhandled error: {}", e.what());
        return httpu::respondError(response, kInternalServerError, "internal server error");
    }
}
