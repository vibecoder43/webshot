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

JobHandler::JobHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : DeadlinedHttpHandler(config, context), crud_(context.FindComponent<Crud>()),
      config_(context.FindComponent<Config>())
{
}

std::string JobHandler::HandleRequestThrowDeadlined(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    HandlerRequestSupport request_support{config_};

    auto uuid = request_support.ParseRequiredPathParamUuid(request, "uuid"_t);
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
