#include "docs_handler.hpp"

#include "deadline_utils.hpp"
#include "integers.hpp"

#include <format>

#include <userver/components/component.hpp>
#include <userver/http/content_type.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace ws {
namespace us = userver;
namespace server = us::server;

DocsHandler::DocsHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : RatelimitedDeadlinedHttpHandler(config, context), title(config["title"].As<std::string>()),
      spec_url(config["spec-url"].As<std::string>())
{
}

us::yaml_config::Schema DocsHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<RatelimitedDeadlinedHttpHandler>(R"(
type: object
description: RapiDoc docs handler
additionalProperties: false
properties:
  title:
    type: string
    description: HTML title used for the RapiDoc page
  spec-url:
    type: string
    description: Same-origin OpenAPI schema URL consumed by RapiDoc
)");
}

std::string DocsHandler::HandleRequestThrowRatelimitedDeadlined(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
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
        title, spec_url
    );
}

} // namespace ws
