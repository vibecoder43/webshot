#include "by_id_handler.hpp"
/**
 * @file
 * @brief Handler that resolves a capture id to JSON metadata.
 */
#include "config.hpp"
#include "crud.hpp"
#include "handler_request_support.hpp"
#include "http.hpp"
#include "storage_url.hpp"
#include "text.hpp"
#include "uuid_utils.hpp"

#include <format>

#include <userver/components/component.hpp>
#include <userver/engine/exception.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/utils/boost_uuid4.hpp>

namespace ws {
namespace us = userver;
namespace server = us::server;
} // namespace ws

using namespace ws;
using namespace text::literals;

ByIdHandler::ByIdHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : DeadlinedHttpHandler(config, context), crud_(context.FindComponent<Crud>()),
      config_(context.FindComponent<Config>())
{
}

std::string ByIdHandler::HandleRequestThrowDeadlined(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    HandlerRequestSupport request_support{crud_, config_};

    const auto uuid = request_support.ParseRequiredPathParamUuid(request, "uuid"_t);
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

    const auto download_url = MakeCaptureDownloadUrl(
        (**capture).uuid, config_.S3Mode(), config_.S3PublicBaseUrl(),
        request_support.RequestHost(request), request_support.RequestForwardedHost(request),
        request_support.RequestForwardedProto(request), config_.HttpsOnly()
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
        (**capture).link.ToBytes(),
        download_url->Href().ToBytes(),
    };
    return httpu::RespondJson(response, kOk, details);
}
