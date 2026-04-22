#include "job_handler.hpp"
/**
 * @file
 * @brief Handler that exposes crawl job status by UUID.
 */
#include "config.hpp"
#include "crud.hpp"
#include "handler_request_support.hpp"
#include "http_utils.hpp"
#include "integers.hpp"
#include "schema/webshot.hpp"
#include "text.hpp"
#include "uuid_utils.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

#include <userver/components/component.hpp>
#include <userver/engine/exception.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/http/common_headers.hpp>
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
using namespace std::chrono_literals;

namespace {

[[nodiscard]] std::string respondJobPollCooldown(
    server::http::HttpResponse &response, const Uuid &uuid, std::chrono::milliseconds retryAfter
)
{
    using enum server::http::HttpStatus;

    const auto retryAfterSeconds = std::chrono::ceil<std::chrono::seconds>(retryAfter).count();
    const auto retryAfterSecondsCount = std::max<i64>(1, i64(retryAfterSeconds));
    dto::CaptureJobCooldownResponse body{
        .uuid = uuid,
        .retry_after_sec = retryAfterSecondsCount,
        .error = dto::CaptureJobCooldownResponse::Error{"client IP in cooldown"},
    };

    response.SetHeader(us::http::headers::kRetryAfter, std::to_string(retryAfterSecondsCount));
    return httpu::respondJson(response, kTooManyRequests, body);
}

} // namespace

JobHandler::JobHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<Crud>()),
      config(context.FindComponent<Config>()),
      requestTimeout(config["request-timeout-ms"].As<int64_t>() * 1ms)
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
        return respondJobPollCooldown(response, *uuid, (*cooldown)->retryAfter);

    auto job = crud.findCaptureJob(*uuid);
    if (!job)
        return httpu::respondError(response, kInternalServerError, "internal server error"_t);
    if (!*job)
        return httpu::respondError(response, kNotFound, "job not found"_t);
    return httpu::respondJson(response, kOk, **job);
}
