/**
 * @file
 * @brief Handler that disallows a host and enqueues purge of its captures.
 */

#include "disallow_and_purge_handler.hpp"
#include "config.hpp"
#include "crud.hpp"
#include "deadline_utils.hpp"
#include "http_utils.hpp"
#include "integers.hpp"
#include "link.hpp"
#include "prefix_utils.hpp"
#include "text.hpp"
#include <boost/safe_numerics/checked_default.hpp>
#include <boost/safe_numerics/checked_result_operations.hpp>
#include <boost/safe_numerics/safe_base_operations.hpp>
#include <boost/safe_numerics/safe_common.hpp>
#include <chrono>
#include <exception>
#include <fmt/format.h>
#include <optional>
#include <stdint.h>
#include <string>
#include <userver/engine/deadline.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/http/status_code.hpp>
#include <userver/logging/level.hpp>
#include <userver/logging/log.hpp>
#include <userver/logging/log_helper.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/zstring_view.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/yaml_config.hpp>

using namespace v1;
using namespace text::literals;
namespace engine = userver::engine;

DisallowAndPurgeHandler::DisallowAndPurgeHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<Crud>()),
      config(context.FindComponent<Config>()),
      requestTimeoutMs(i64(config["request-timeout-ms"].As<int64_t>()))
{
}

us::yaml_config::Schema DisallowAndPurgeHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: Disallow_and_purge handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /v1/denylist/disallow_and_purge handler in milliseconds
)");
}

std::string DisallowAndPurgeHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using server::http::HttpStatus::kAccepted;
    using server::http::HttpStatus::kBadRequest;
    using server::http::HttpStatus::kInternalServerError;

    auto &response = request.GetHttpResponse();
    try {
        const auto handlerTimeout = std::chrono::milliseconds{requestTimeoutMs};
        auto finalDeadline = computeHandlerDeadline(request, handlerTimeout);
        engine::current_task::SetDeadline(finalDeadline);
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("Failed to compute handler deadline: {}", e.what());
        return httpu::respondError(response, kInternalServerError, "internal server error"_t);
    }

    const std::string arg = request.GetArg("host");
    if (arg.empty())
        return httpu::respondParamError(response, kBadRequest, "host"_t, "missing parameter"_t);
    const auto host = String::fromBytes(arg);
    if (!host)
        return httpu::respondParamError(response, kBadRequest, "host"_t, "invalid parameter"_t);
    std::optional<Link> link;
    try {
        link = Link::fromTextStripPortQuery(*host, config.queryPartLengthMax());
    } catch (const InvalidLinkException &e) {
        LOG_INFO() << fmt::format("invalid host: {}", e.what());
        return httpu::respondParamError(response, kBadRequest, "host"_t, "invalid parameter"_t);
    }
    LOG_INFO() << fmt::format("invoked for: {}", link->host());
    try {
        auto prefixKey = prefix::makePrefixKey(*link);
        crud.disallowAndPurgePrefix(prefixKey);
        response.SetStatus(kAccepted);
        return {};
    } catch (const std::exception &e) {
        LOG_CRITICAL() << fmt::format("failed for {}: {}", link->host(), e.what());
        us::utils::AbortWithStacktrace("disallowing host failed");
    }
}
