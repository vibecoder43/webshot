#include "ui_redirect_handler.hpp"
/**
 * @file
 * @brief Redirect /ui to /ui/ for static UI serving under /ui/...
 */
#include "deadline_utils.hpp"
#include "integers.hpp"

#include <chrono>

#include <userver/engine/task/current_task.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace v1 {

UiRedirectHandler::UiRedirectHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context),
      requestTimeoutMs(i64(config["request-timeout-ms"].As<int64_t>()))
{
}

us::yaml_config::Schema UiRedirectHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: Redirect /ui -> /ui/
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /ui handler in milliseconds
)");
}

std::string UiRedirectHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    const auto handlerTimeout = std::chrono::milliseconds{requestTimeoutMs};
    auto finalDeadline = computeHandlerDeadline(request, handlerTimeout);
    eng::current_task::SetDeadline(finalDeadline);

    response.SetStatus(kFound);
    response.SetHeader(us::http::headers::kLocation, "/ui/");
    response.SetContentType("text/plain");
    return {};
}

} // namespace v1
