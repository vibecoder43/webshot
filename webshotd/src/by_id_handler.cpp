#include "by_id_handler.hpp"
/**
 * @file
 * @brief Handler that resolves a capture id to JSON metadata.
 */
#include "client_ip.hpp"
#include "config.hpp"
#include "crud.hpp"
#include "deadline_utils.hpp"
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
    auto finalDeadline = computeHandlerDeadline(request, requestTimeout);
    eng::current_task::SetDeadline(finalDeadline);

    const std::string arg = request.GetPathArg("uuid");
    if (arg.empty())
        return httpu::respondParamError(response, kBadRequest, "uuid"_t, "missing parameter"_t);

    const auto uuidStr = String::fromBytes(arg);
    if (!uuidStr)
        return httpu::respondParamError(response, kBadRequest, "uuid"_t, "invalid parameter"_t);

    const auto uuidOpt = uuidu::parse(uuidStr->view());
    if (!uuidOpt)
        return httpu::respondParamError(response, kBadRequest, "uuid"_t, "invalid parameter"_t);

    auto clientIp = client_ip::resolve(request, config);
    if (!clientIp)
        return httpu::respondError(response, kBadRequest, "invalid client ip"_t);
    auto cooldown = *crud.acquireClientIpCooldown(std::move(*clientIp));
    if (cooldown)
        return httpu::respondClientIpCooldown(response, cooldown->retryAfter);

    auto capture = crud.findCapture(*uuidOpt);
    if (!capture)
        return httpu::respondError(response, kInternalServerError, "internal server error"_t);
    if (!*capture) {
        LOG_INFO() << std::format("capture not found: {}", *uuidOpt);
        return httpu::respondError(response, kNotFound, "capture not found"_t);
    }

    return httpu::respondJson(
        response, kOk,
        dto::CaptureDetails{
            (**capture).uuid,
            (**capture).createdAt,
            std::string((**capture).link.view()),
            std::string(buildCaptureDownloadUrl((**capture).uuid, config).href().view()),
        }
    );
}
