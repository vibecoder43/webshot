#include "webshot_by_id_handler.hpp"
/**
 * @file
 * @brief Handler that resolves a capture id to its public location via 302.
 */
#include "deadline_utils.hpp"
#include "http_utils.hpp"
#include "text.hpp"
#include "webshot_crud.hpp"

#include <chrono>

#include <exception>
#include <fmt/format.h>

#include <userver/components/component.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/http/content_type.hpp>
#include <userver/http/status_code.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/utils/boost_uuid4.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

using namespace v1;
using namespace text::literals;
namespace engine = userver::engine;

WebshotById::WebshotById(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<WebshotCrud>()),
      requestTimeoutMs(config["request-timeout-ms"].As<int64_t>())
{
}

us::yaml_config::Schema WebshotById::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: Webshot_by_id handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /v1/webshot/{uuid} handler in milliseconds
)");
}

std::string WebshotById::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using server::http::HttpStatus::kBadRequest;
    using server::http::HttpStatus::kFound;
    using server::http::HttpStatus::kInternalServerError;
    using server::http::HttpStatus::kNotFound;
    using us::http::content_type::kApplicationJson;

    auto &response = request.GetHttpResponse();
    try {
        const auto handlerTimeout = std::chrono::milliseconds(requestTimeoutMs);
        auto finalDeadline = computeHandlerDeadline(request, handlerTimeout);
        engine::current_task::SetDeadline(finalDeadline);

        const std::string arg = request.GetPathArg("uuid");
        if (arg.empty())
            return httpu::respondParamError(response, kBadRequest, "uuid"_t, "missing parameter"_t);
        const auto uuidStr = String::fromBytes(arg);
        if (!uuidStr)
            return httpu::respondParamError(response, kBadRequest, "uuid"_t, "invalid parameter"_t);
        Uuid uuid;
        try {
            uuid = us::utils::BoostUuidFromString(uuidStr->view());
        } catch (std::exception &e) {
            return httpu::respondParamError(response, kBadRequest, "uuid"_t, "invalid parameter"_t);
        }
        auto location = crud.findWebshot(uuid);
        if (!location) {
            LOG_INFO() << fmt::format("webshot not found: {}", us::utils::ToString(uuid));
            return httpu::respondError(response, kNotFound, "webshot not found"_t);
        }
        response.SetStatus(kFound);
        response.SetHeader(us::http::headers::kLocation, std::string(location->httpsUrl().view()));
        return {};
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("Unhandled error: {}", e.what());
        return httpu::respondError(response, kInternalServerError, "internal server error"_t);
    }
}
