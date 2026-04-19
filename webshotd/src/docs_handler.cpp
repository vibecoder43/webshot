#include "docs_handler.hpp"

#include "deadline_utils.hpp"
#include "integers.hpp"

#include <chrono>
#include <format>

#include <userver/components/component.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/http/content_type.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace v1 {
using namespace std::chrono_literals;

DocsHandler::DocsHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context),
      requestTimeout(config["request-timeout-ms"].As<int64_t>() * 1ms),
      title(config["title"].As<std::string>()), specUrl(config["spec-url"].As<std::string>())
{
}

us::yaml_config::Schema DocsHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: RapiDoc docs handler
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /rapidoc handler in milliseconds
  title:
    type: string
    description: HTML title used for the RapiDoc page
  spec-url:
    type: string
    description: Same-origin OpenAPI schema URL consumed by RapiDoc
)");
}

std::string DocsHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    auto finalDeadline = computeHandlerDeadline(request, requestTimeout);
    eng::current_task::SetDeadline(finalDeadline);

    auto &response = request.GetHttpResponse();
    response.SetStatus(server::http::HttpStatus::kOk);
    response.SetContentType("text/html");
    return std::format(
        R"(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{}</title>
  <script src="/rapidoc-assets/rapidoc-min.js"></script>
</head>
<body>
  <rapi-doc spec-url="{}"></rapi-doc>
</body>
</html>
)",
        title, specUrl
    );
}

} // namespace v1
