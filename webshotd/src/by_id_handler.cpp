#include "by_id_handler.hpp"
/**
 * @file
 * @brief Handler that resolves a capture id to JSON metadata.
 */
#include "config.hpp"
#include "crud.hpp"
#include "handler_request_support.hpp"
#include "http_utils.hpp"
#include "integers.hpp"
#include "text.hpp"
#include "uuid_format.hpp"
#include "uuid_utils.hpp"

#include <chrono>

#include <format>
#include <utility>

#include <userver/components/component.hpp>
#include <userver/engine/exception.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/utils/boost_uuid4.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

using namespace v1;
using namespace text::literals;
using namespace std::chrono_literals;

ById::ById(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<Crud>()),
      config(context.FindComponent<Config>()),
      requestTimeout(config["request-timeout-ms"].As<int64_t>() * 1ms)
{
}

us::yaml_config::Schema ById::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: By_id handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /v1/capture/{uuid} handler in milliseconds
)");
}

std::string ById::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    HandlerRequestSupport requestSupport{crud, config};
    requestSupport.applyRequestDeadline(request, requestTimeout);

    const auto uuid = requestSupport.parseUuidPathArg(request, "uuid"_t);
    if (!uuid)
        return httpu::respondParamError(
            response, kBadRequest, uuid.error().name, uuid.error().message
        );

    const auto cooldown = requestSupport.checkClientIpCooldown(request);
    if (!cooldown)
        return respondClientRequestError(response, cooldown.error());
    if (*cooldown)
        return httpu::respondClientIpCooldown(response, (*cooldown)->retryAfter);

    auto capture = crud.findCapture(*uuid);
    if (!capture)
        return httpu::respondError(response, kInternalServerError, "internal server error"_t);
    if (!*capture) {
        LOG_INFO() << std::format("capture not found: {}", *uuid);
        return httpu::respondError(response, kNotFound, "capture not found"_t);
    }

    return httpu::respondJson(
        response, kOk,
        dto::CaptureDetails{
            (**capture).uuid,
            (**capture).createdAt,
            std::to_string((**capture).link),
            std::to_string(buildCaptureDownloadUrl((**capture).uuid, config).href()),
        }
    );
}
