#include "docs_handler.hpp"
#include "deadline_utils.hpp"
#include "integers.hpp"
#include <boost/safe_numerics/checked_default.hpp>
#include <boost/safe_numerics/checked_result_operations.hpp>
#include <boost/safe_numerics/safe_base_operations.hpp>
#include <boost/safe_numerics/safe_common.hpp>
#include <chrono>
#include <stdint.h>
#include <userver/engine/deadline.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/http/status_code.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/yaml_config.hpp>

namespace v1 {
namespace engine = userver::engine;

DocsHandler::DocsHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context),
      requestTimeoutMs(i64(config["request-timeout-ms"].As<int64_t>()))
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
)");
}

std::string DocsHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    const auto handlerTimeout = std::chrono::milliseconds{requestTimeoutMs};
    auto finalDeadline = computeHandlerDeadline(request, handlerTimeout);
    engine::current_task::SetDeadline(finalDeadline);

    auto &response = request.GetHttpResponse();
    response.SetStatus(server::http::HttpStatus::kOk);
    response.SetContentType("text/html");
    return R"(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Webshot API</title>
  <script src="/rapidoc-assets/rapidoc-min.js"></script>
</head>
<body>
  <rapi-doc spec-url="/openapi/webshot.yaml"></rapi-doc>
</body>
</html>
)";
}

} // namespace v1
