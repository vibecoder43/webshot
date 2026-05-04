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
#include "storage_url.hpp"
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

namespace ws {
namespace us = userver;
namespace server = us::server;
} // namespace ws

using namespace ws;
using namespace text::literals;
using namespace std::chrono_literals;
using text::ToBytes;

ById::ById(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud_(context.FindComponent<Crud>()),
      config_(context.FindComponent<Config>()),
      request_timeout(config["request-timeout-ms"].As<int64_t>() * 1ms)
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
    description: Upper bound for /ws/capture/{uuid} handler in milliseconds
)");
}

std::string ById::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    HandlerRequestSupport request_support{crud_, config_};
    request_support.ApplyRequestDeadline(request, request_timeout);

    const auto uuid = request_support.ParseUuidPathArg(request, "uuid"_t);
    if (!uuid)
        return httpu::RespondParamError(
            response, kBadRequest, uuid.Error().name, uuid.Error().message
        );

    const auto cooldown = request_support.CheckClientIpCooldown(request);
    if (!cooldown)
        return RespondClientRequestError(response, cooldown.Error());
    if (*cooldown)
        return httpu::RespondClientIpCooldown(response, (*cooldown)->retry_after);

    auto capture = crud_.FindCapture(*uuid);
    if (!capture)
        return httpu::RespondError(response, kInternalServerError, "internal server error"_t);
    if (!*capture) {
        LOG_INFO() << std::format("capture not found: {}", *uuid);
        return httpu::RespondError(response, kNotFound, "capture not found"_t);
    }

    const auto download_url = BuildCaptureDownloadUrl(
        (**capture).uuid, config_.S3Mode(), config_.S3PublicBaseUrl(),
        request_support.RequestHost(request)
    );
    if (!download_url) {
        LOG_ERROR() << std::format(
            "Failed to build storage_url: {}", StorageUrlErrorMessage(download_url.Error())
        );
        return httpu::RespondError(response, kInternalServerError, "internal server error"_t);
    }

    dto::CaptureDetails details{
        (**capture).uuid,
        (**capture).created_at,
        ToBytes((**capture).link),
        ToBytes(download_url->Href()),
    };
    return httpu::RespondJson(response, kOk, details);
}
