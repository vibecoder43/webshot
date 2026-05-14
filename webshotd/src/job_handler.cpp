#include "job_handler.hpp"
/**
 * @file
 * @brief Handler that exposes crawl job status by UUID.
 */
#include "config.hpp"
#include "crud.hpp"
#include "handler_request_support.hpp"
#include "http.hpp"
#include "integers.hpp"
#include "schema/public/webshot.hpp"
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

namespace ws {
namespace us = userver;
namespace server = us::server;
} // namespace ws

using namespace ws;
using namespace text::literals;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] std::string RespondJobPollRatelimit(
    server::http::HttpResponse &response, const Uuid &uuid, std::chrono::milliseconds retry_after
)
{
    using enum server::http::HttpStatus;

    const auto retry_after_seconds = std::max(
        1s, std::chrono::ceil<std::chrono::seconds>(retry_after)
    );
    dto::CaptureJobRatelimitResponse body{
        .uuid = uuid,
        .retry_after_sec = i64{retry_after_seconds.count()},
        .error = dto::CaptureJobRatelimitResponse::Error{"client IP rate limited"},
    };

    response.SetHeader(us::http::headers::kRetryAfter, std::to_string(retry_after_seconds.count()));
    return httpu::RespondJson(response, kTooManyRequests, body);
}

} // namespace

JobHandler::JobHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : RatelimitedDeadlinedHttpHandler(config, context)
{
}

std::string JobHandler::HandleRequestThrowRatelimitedDeadlined(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    HandlerRequestSupport request_support{config_};

    const auto uuid = request_support.ParseRequiredPathParamUuid(request, "uuid"_t);
    if (!uuid)
        return httpu::RespondParamError(
            response, kBadRequest, uuid.Error().name, uuid.Error().message
        );

    auto job = crud_.FindCaptureJob(*uuid);
    if (!job)
        return httpu::RespondError(response, kInternalServerError, "internal server error"_t);
    if (!*job)
        return httpu::RespondError(response, kNotFound, "job not found"_t);
    return httpu::RespondJson(response, kOk, **job);
}

std::string JobHandler::RespondClientIpRatelimit(
    const server::http::HttpRequest &request, std::chrono::milliseconds retry_after
) const
{
    using enum server::http::HttpStatus;
    using namespace text::literals;

    auto &response = request.GetHttpResponse();
    HandlerRequestSupport request_support{config_};

    const auto uuid = request_support.ParseRequiredPathParamUuid(request, "uuid"_t);
    if (!uuid)
        return httpu::RespondParamError(
            response, kBadRequest, uuid.Error().name, uuid.Error().message
        );

    return RespondJobPollRatelimit(response, *uuid, retry_after);
}
