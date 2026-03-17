#include "denylist_check_handler.hpp"
/**
 * @file
 * @brief Internal endpoint for checking whether a URL is denylisted.
 */
#include "config.hpp"
#include "deadline_utils.hpp"
#include "denylist.hpp"
#include "integers.hpp"
#include "link.hpp"
#include "prefix_utils.hpp"

#include <chrono>
#include <exception>
#include <optional>
#include <string>

#include <fmt/format.h>

#include <userver/components/component.hpp>
#include <userver/engine/exception.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/server/http/http_method.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace engine = userver::engine;

namespace v1 {

DenylistCheckHandler::DenylistCheckHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), config(context.FindComponent<Config>()),
      denylist(context.FindComponent<Denylist>()),
      requestTimeoutMs(i64(config["request-timeout-ms"].As<int64_t>()))
{
}

us::yaml_config::Schema DenylistCheckHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: Denylist check handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /v1/denylist/check handler in milliseconds
)");
}

std::string DenylistCheckHandler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using server::http::HttpMethod::kPost;
    using server::http::HttpStatus::kBadRequest;
    using server::http::HttpStatus::kForbidden;
    using server::http::HttpStatus::kInternalServerError;
    using server::http::HttpStatus::kMethodNotAllowed;
    using server::http::HttpStatus::kNoContent;

    auto &response = request.GetHttpResponse();

    try {
        const auto handlerTimeout = std::chrono::milliseconds{requestTimeoutMs};
        auto finalDeadline = computeHandlerDeadline(request, handlerTimeout);
        engine::current_task::SetDeadline(finalDeadline);

        if (request.GetMethod() != kPost) {
            response.SetStatus(kMethodNotAllowed);
            return {};
        }

        auto body = String::fromBytes(request.RequestBody());
        if (!body || body->view().empty()) {
            response.SetStatus(kBadRequest);
            return {};
        }

        std::optional<Link> link;
        try {
            link = Link::fromTextStripPort(body.value(), config.queryPartLengthMax());
        } catch (const InvalidLinkException &) {
            response.SetStatus(kBadRequest);
            return {};
        }

        auto prefixKey = prefix::makePrefixKey(link.value());
        if (!denylist.isAllowedPrefix(prefixKey)) {
            response.SetStatus(kForbidden);
            return {};
        }

        response.SetStatus(kNoContent);
        return {};
    } catch (const engine::WaitInterruptedException &) {
        throw;
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("Unhandled error in denylist check handler: {}", e.what());
        response.SetStatus(kInternalServerError);
        return {};
    }
}

} // namespace v1
