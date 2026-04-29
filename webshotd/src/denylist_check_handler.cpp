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

namespace v1 {
using namespace std::chrono_literals;

DenylistCheckHandler::DenylistCheckHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), config(context.FindComponent<Config>()),
      denylist(context.FindComponent<Denylist>()), metrics(context.FindComponent<Metrics>()),
      requestTimeout(config["request-timeout-ms"].As<int64_t>() * 1ms)
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
    description: Upper bound for /v1/denylist/check handler in milliseconds
)");
}

std::string DenylistCheckHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using server::http::HttpMethod::kPost;
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();

    auto finalDeadline = computeHandlerDeadline(request, requestTimeout);
    eng::current_task::SetDeadline(finalDeadline);

    if (request.GetMethod() != kPost) {
        response.SetStatus(kMethodNotAllowed);
        return {};
    }

    const auto link = parseJsonLinkBody(request, config);
    if (!link)
        return httpu::respondError(response, kBadRequest, link.error());

    auto prefixKey = prefix::makePrefixKey(*link);
    const auto allowed = denylist.isAllowedPrefix(prefixKey);
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

} // namespace v1
