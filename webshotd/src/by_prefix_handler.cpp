#include "by_prefix_handler.hpp"
/**
 * @file
 * @brief Handler that lists captures grouped by normalized link prefix.
 */
#include "config.hpp"
#include "crud.hpp"
#include "handler_request_support.hpp"
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

namespace ws {
namespace us = userver;
namespace server = us::server;
} // namespace ws

using namespace ws;
using namespace text::literals;
using namespace std::chrono_literals;

ByPrefixHandler::ByPrefixHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud_(context.FindComponent<Crud>()),
      cfg_(context.FindComponent<Config>()),
      request_timeout(config["request-timeout-ms"].As<int64_t>() * 1ms)
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
    description: Upper bound for /ws/capture/prefix handler in milliseconds
)");
}

std::string ByPrefixHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;
    auto &response = request.GetHttpResponse();
    HandlerRequestSupport request_support{crud_, cfg_};
    request_support.ApplyRequestDeadline(request, request_timeout);

    const auto prefix = request_support.ParseRequiredQueryLink(request, "prefix"_t);
    if (!prefix)
        return httpu::RespondParamError(
            response, kBadRequest, prefix.Error().name, prefix.Error().message
        );

    const auto token = request_support.ParseQueryText(request, "page_token"_t);
    if (!token)
        return httpu::RespondParamError(
            response, kBadRequest, token.Error().name, token.Error().message
        );

    const auto cooldown = request_support.CheckClientIpCooldown(request);
    if (!cooldown)
        return RespondClientRequestError(response, cooldown.Error());
    if (*cooldown)
        return httpu::RespondClientIpCooldown(response, (*cooldown)->retry_after);

    auto page = crud_.FindCapturesByPrefixPage(prefix->Normalized(), *token);
    if (!page) {
        using enum errors::CapturePageError;
        if (page.Error() == kDbFailure)
            return httpu::RespondError(response, kInternalServerError, "internal server error"_t);
        return httpu::RespondParamError(
            response, kBadRequest, "page_token"_t, "invalid page_token"_t
        );
    }
    return httpu::RespondJson(response, kOk, *page);
}
