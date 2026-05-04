#include "denylist_check_handler.hpp"
/**
 * @file
 * @brief Internal endpoint for checking whether a URL is denylisted.
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

#include <chrono>
#include <format>
#include <optional>
#include <string>

#include <userver/components/component.hpp>
#include <userver/engine/exception.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
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

DenylistCheckHandler::DenylistCheckHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), config_(context.FindComponent<Config>()),
      denylist_(context.FindComponent<Denylist>()), metrics_(context.FindComponent<Metrics>()),
      request_timeout(config["request-timeout-ms"].As<int64_t>() * 1ms)
{
}

us::yaml_config::Schema DenylistCheckHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: Denylist check handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /ws/denylist/check handler in milliseconds
)");
}

std::string DenylistCheckHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using server::http::HttpMethod::kPost;
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();

    auto final_deadline = ComputeHandlerDeadline(request, request_timeout);
    eng::current_task::SetDeadline(final_deadline);

    if (request.GetMethod() != kPost) {
        response.SetStatus(kMethodNotAllowed);
        return {};
    }

    const auto link = ParseJsonLinkBody(request, config_);
    if (!link)
        return httpu::RespondError(response, kBadRequest, link.Error());

    auto prefix_key = prefix::MakePrefixKey(*link);
    const auto allowed = denylist_.IsAllowedPrefix(prefix_key);
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

} // namespace ws
