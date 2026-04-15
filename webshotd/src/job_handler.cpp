#include "job_handler.hpp"
/**
 * @file
 * @brief Handler that exposes crawl job status by UUID.
 */
#include "client_ip.hpp"
#include "config.hpp"
#include "crud.hpp"
#include "deadline_utils.hpp"
#include "http_utils.hpp"
#include "integers.hpp"
#include "text.hpp"

#include <chrono>
#include <format>
#include <optional>
#include <utility>

#include <boost/uuid/string_generator.hpp>

#include <userver/components/component.hpp>
#include <userver/engine/exception.hpp>
#include <userver/engine/task/current_task.hpp>
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

JobHandler::JobHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<Crud>()),
      config(context.FindComponent<Config>()),
      requestTimeoutMs(i64(config["request-timeout-ms"].As<int64_t>()))
{
}

us::yaml_config::Schema JobHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: Job handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /v1/capture/jobs/{uuid} handler in milliseconds
)");
}

std::string JobHandler::HandleRequestThrow(
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

    auto uuidStr = String::fromBytes(arg);
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

    auto job = crud.findCaptureJob(uuidOpt.value());
    if (!job)
        return httpu::respondError(response, kInternalServerError, "internal server error"_t);
    if (!job.value())
        return httpu::respondError(response, kNotFound, "job not found"_t);
    return httpu::respondJson(response, kOk, job.value().value());
}
