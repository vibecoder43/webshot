#include "by_prefix_handler.hpp"
/**
 * @file
 * @brief Handler that lists captures grouped by normalized link prefix.
 */
#include "config.hpp"
#include "crud.hpp"
#include "deadline_utils.hpp"
#include "integers.hpp"
#include "link.hpp"
#include "text.hpp"

#include <chrono>
#include <string>

#include "http_utils.hpp"
#include "server_errors.hpp"
#include <userver/components/component.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/formats/json.hpp>
#include <userver/http/content_type.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/http/http_method.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace us = userver;

using namespace v1;
using namespace text::literals;
namespace engine = userver::engine;

ByPrefixHandler::ByPrefixHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<Crud>()),
      cfg(context.FindComponent<Config>()),
      requestTimeoutMs(i64(config["request-timeout-ms"].As<int64_t>()))
{
}

us::yaml_config::Schema ByPrefixHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: By_prefix handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /v1/capture/prefix handler in milliseconds
)");
}

std::string ByPrefixHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using server::http::HttpStatus::kBadRequest;
    using server::http::HttpStatus::kInternalServerError;
    using server::http::HttpStatus::kMethodNotAllowed;
    using server::http::HttpStatus::kOk;
    using us::http::content_type::kApplicationJson;
    auto &response = request.GetHttpResponse();
    try {
        const auto handlerTimeout = std::chrono::milliseconds{requestTimeoutMs};
        auto finalDeadline = computeHandlerDeadline(request, handlerTimeout);
        engine::current_task::SetDeadline(finalDeadline);

        const std::string arg = request.GetArg("prefix");
        if (arg.empty())
            return httpu::respondParamError(
                response, kBadRequest, "prefix"_t, "missing parameter"_t
            );
        auto prefix = String::fromBytes(arg);
        if (!prefix)
            return httpu::respondParamError(
                response, kBadRequest, "prefix"_t, "invalid parameter"_t
            );
        String normalizedPrefix;
        try {
            normalizedPrefix =
                Link::fromTextStripPort(prefix.value(), cfg.queryPartLengthMax()).normalized();
        } catch (const InvalidLinkException &e) {
            return httpu::respondError(response, kBadRequest, String::fromBytesThrow(e.what()));
        }
        const std::string tokenArg = request.GetArg("page_token");
        const auto token = String::fromBytes(tokenArg);
        if (!token)
            return httpu::respondParamError(
                response, kBadRequest, "page_token"_t, "invalid parameter"_t
            );
        try {
            auto page = crud.findCapturesByPrefixPage(normalizedPrefix, token.value());
            return httpu::respondJson(response, kOk, page);
        } catch (const errors::InvalidPageTokenException &) {
            return httpu::respondParamError(
                response, kBadRequest, "page_token"_t, "invalid page_token"_t
            );
        }
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("Unhandled error: {}", e.what());
        return httpu::respondError(response, kInternalServerError, "internal server error"_t);
    }
}
