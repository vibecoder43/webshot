#include "handler.hpp"
/**
 * @file
 * @brief Handler that creates captures and lists them by exact link.
 */
#include "config.hpp"
#include "crud.hpp"
#include "deadline_utils.hpp"
#include "denylist.hpp"
#include "http_utils.hpp"
#include "integers.hpp"
#include "link.hpp"
#include "prefix_utils.hpp"
#include "schema/webshot.hpp"
#include "server_errors.hpp"
#include "text.hpp"

#include <chrono>
#include <exception>
#include <optional>
#include <string>

#include <fmt/format.h>

#include <userver/components/component.hpp>
#include <userver/engine/exception.hpp>
#include <userver/engine/task/current_task.hpp>
#include <userver/formats/json.hpp>
#include <userver/formats/serialize/common_containers.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/http/content_type.hpp>
#include <userver/http/status_code.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/server/http/http_method.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/utils/boost_uuid4.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

using namespace v1;
using namespace text::literals;
namespace json = userver::formats::json;
namespace engine = userver::engine;

Handler::Handler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<Crud>()),
      config(context.FindComponent<Config>()), denylist(context.FindComponent<Denylist>()),
      requestTimeoutMs(i64(config["request-timeout-ms"].As<int64_t>()))
{
}

us::yaml_config::Schema Handler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: Handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /v1/capture handler in milliseconds
)");
}

std::string Handler::HandleRequestThrow(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using server::http::HttpMethod::kPost;
    using server::http::HttpStatus::kAccepted;
    using server::http::HttpStatus::kBadRequest;
    using server::http::HttpStatus::kForbidden;
    using server::http::HttpStatus::kInternalServerError;
    using server::http::HttpStatus::kOk;

    auto &response = request.GetHttpResponse();
    try {
        const auto handlerTimeout = std::chrono::milliseconds{requestTimeoutMs};
        auto finalDeadline = computeHandlerDeadline(request, handlerTimeout);
        engine::current_task::SetDeadline(finalDeadline);

        if (request.GetMethod() == kPost) {
            dto::CreateCaptureRequest req;
            try {
                auto str = String::fromBytes(request.RequestBody());
                if (!str)
                    throw std::exception();
                req = json::FromString(str->view()).As<dto::CreateCaptureRequest>();
            } catch (const std::exception &e) {
                return httpu::respondError(response, kBadRequest, "invalid request body"_t);
            }
            try {
                auto parsed = Link::fromTextStripPort(
                    String::fromBytesThrow(req.link), config.queryPartLengthMax()
                );
                auto prefixKey = prefix::makePrefixKey(parsed);
                if (!denylist.isAllowedPrefix(prefixKey))
                    return httpu::respondError(response, kForbidden, "host in denylist"_t);
                auto job = crud.createCaptureJob(std::move(parsed));
                return httpu::respondJson(response, kAccepted, job);
            } catch (const InvalidLinkException &e) {
                return httpu::respondError(response, kBadRequest, String::fromBytesThrow(e.what()));
            }
        }

        const std::string arg = request.GetArg("link");
        if (arg.empty())
            return httpu::respondParamError(response, kBadRequest, "link"_t, "missing parameter"_t);
        auto str = String::fromBytes(arg);
        if (!str)
            return httpu::respondParamError(response, kBadRequest, "link"_t, "invalid parameter"_t);
        std::optional<Link> link;
        try {
            link = Link::fromTextStripPort(str.value(), config.queryPartLengthMax());
        } catch (const InvalidLinkException &e) {
            return httpu::respondError(response, kBadRequest, String::fromBytesThrow(e.what()));
        }
        const std::string tokenArg = request.GetArg("page_token");
        const auto token = String::fromBytes(tokenArg);
        if (!token)
            return httpu::respondParamError(
                response, kBadRequest, "page_token"_t, "missing parameter"_t
            );
        try {
            auto page = crud.findCapturesByLinkPage(link.value(), token.value());
            return httpu::respondJson(response, kOk, page);
        } catch (const errors::InvalidPageTokenException &) {
            return httpu::respondParamError(
                response, kBadRequest, "page_token"_t, "invalid page_token"_t
            );
        }
    } catch (const engine::WaitInterruptedException &) {
        throw;
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("Unhandled error in handler: {}", e.what());
        return httpu::respondError(response, kInternalServerError, "internal server error"_t);
    }
}
