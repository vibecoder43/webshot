#include "job_handler.hpp"
/**
 * @file
 * @brief Handler that exposes crawl job status by UUID.
 */
#include "crud.hpp"
#include "deadline_utils.hpp"
#include "http_utils.hpp"
#include "integers.hpp"
#include "text.hpp"

#include <chrono>

#include <fmt/format.h>

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
namespace engine = userver::engine;

JobHandler::JobHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<Crud>()),
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
    using server::http::HttpStatus::kBadRequest;
    using server::http::HttpStatus::kInternalServerError;
    using server::http::HttpStatus::kNotFound;
    using server::http::HttpStatus::kOk;

    auto &response = request.GetHttpResponse();
    try {
        const auto handlerTimeout = std::chrono::milliseconds{requestTimeoutMs};
        auto finalDeadline = computeHandlerDeadline(request, handlerTimeout);
        engine::current_task::SetDeadline(finalDeadline);

        const std::string arg = request.GetPathArg("uuid");
        if (arg.empty())
            return httpu::respondParamError(response, kBadRequest, "uuid"_t, "missing parameter"_t);
        auto uuidStr = String::fromBytes(arg);
        if (!uuidStr)
            return httpu::respondParamError(response, kBadRequest, "uuid"_t, "invalid parameter"_t);
        Uuid uuid;
        try {
            uuid = us::utils::BoostUuidFromString(uuidStr->view());
        } catch (const std::exception &) {
            return httpu::respondParamError(response, kBadRequest, "uuid"_t, "invalid parameter"_t);
        }
        auto job = crud.findCaptureJob(uuid);
        if (!job)
            return httpu::respondError(response, kNotFound, "job not found"_t);
        return httpu::respondJson(response, kOk, job.value());
    } catch (const engine::WaitInterruptedException &) {
        throw;
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("Unhandled error in job_handler: {}", e.what());
        return httpu::respondError(response, kInternalServerError, "internal server error"_t);
    }
}
