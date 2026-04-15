#include "by_id_handler.hpp"
/**
 * @file
 * @brief Handler that resolves a capture id to its public location via 302.
 */
#include "client_ip.hpp"
#include "config.hpp"
#include "crud.hpp"
#include "deadline_utils.hpp"
#include "http_utils.hpp"
#include "integers.hpp"
#include "text.hpp"
#include "uuid_format.hpp"

#include <chrono>

#include <format>
#include <optional>
#include <utility>

#include <boost/uuid/string_generator.hpp>

#include <userver/components/component.hpp>
#include <userver/engine/exception.hpp>
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

namespace {

[[nodiscard]] std::optional<Uuid> parseUuid(std::string_view text) noexcept
{
    boost::uuids::string_generator gen;
    try {
        return gen(std::string{text});
    } catch (const std::runtime_error &) {
        return {};
    }
}

} // namespace

ById::ById(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<Crud>()),
      config(context.FindComponent<Config>()),
      requestTimeoutMs(i64(config["request-timeout-ms"].As<int64_t>()))
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
    const auto handlerTimeout = std::chrono::milliseconds{requestTimeoutMs};
    auto finalDeadline = computeHandlerDeadline(request, handlerTimeout);
    eng::current_task::SetDeadline(finalDeadline);

    const std::string arg = request.GetPathArg("uuid");
    if (arg.empty())
        return httpu::respondParamError(response, kBadRequest, "uuid"_t, "missing parameter"_t);

    const auto uuidStr = String::fromBytes(arg);
    if (!uuidStr)
        return httpu::respondParamError(response, kBadRequest, "uuid"_t, "invalid parameter"_t);

    const auto uuidOpt = parseUuid(uuidStr->view());
    if (!uuidOpt)
        return httpu::respondParamError(response, kBadRequest, "uuid"_t, "invalid parameter"_t);

    auto clientIp = client_ip::resolve(request, config);
    if (!clientIp)
        return httpu::respondError(response, kBadRequest, "invalid client ip"_t);
    auto cooldown = crud.acquireClientIpCooldown(std::move(clientIp).value()).value();
    if (cooldown)
        return httpu::respondClientIpCooldown(response, cooldown->retryAfter);

    auto location = crud.findCapture(uuidOpt.value());
    if (!location)
        return httpu::respondError(response, kInternalServerError, "internal server error"_t);
    if (!location.value()) {
        LOG_INFO() << std::format("capture not found: {}", uuidOpt.value());
        return httpu::respondError(response, kNotFound, "capture not found"_t);
    }

    response.SetStatus(kFound);
    response.SetHeader(
        us::http::headers::kLocation, std::string(location.value()->httpsUrl().view())
    );
    return {};
}
