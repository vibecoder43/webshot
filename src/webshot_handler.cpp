#include "include/webshot_handler.hpp"
/**
 * @file
 * @brief Handler that creates captures and lists them by exact link.
 */
#include "include/deadline_utils.hpp"
#include "include/host_policy.hpp"
#include "include/http_utils.hpp"
#include "include/link.hpp"
#include "include/server_errors.hpp"
#include "include/webshot_config.hpp"
#include "include/webshot_crud.hpp"
#include "include/webshot_denylist.hpp"
#include "schemas/webshot.hpp"

#include <chrono>
#include <string>

#include <fmt/format.h>

#include <userver/clients/dns/component.hpp>
#include <userver/components/component.hpp>
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
namespace json = userver::formats::json;
namespace engine = userver::engine;

WebshotHandler::WebshotHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<WebshotCrud>()),
      config(context.FindComponent<WebshotConfig>()),
      resolver(context.FindComponent<userver::clients::dns::Component>().GetResolver()),
      denylist(context.FindComponent<WebshotDenylist>()),
      requestTimeoutMs(config["request-timeout-ms"].As<int64_t>())
{
}

us::yaml_config::Schema WebshotHandler::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<server::handlers::HttpHandlerBase>(R"(
type: object
description: Webshot handler static config
additionalProperties: false
properties:
  request-timeout-ms:
    type: integer
    minimum: 1
    description: Upper bound for /v1/webshot handler in milliseconds
)");
}

std::string WebshotHandler::
    HandleRequestThrow(const server::http::HttpRequest &request, server::request::RequestContext &)
        const
{
    using server::http::HttpMethod::kPost;
    using server::http::HttpStatus::kBadRequest;
    using server::http::HttpStatus::kCreated;
    using server::http::HttpStatus::kForbidden;
    using server::http::HttpStatus::kInternalServerError;
    using server::http::HttpStatus::kOk;

    auto &response = request.GetHttpResponse();
    try {
        const auto handlerTimeout = std::chrono::milliseconds(requestTimeoutMs);
        auto finalDeadline = computeHandlerDeadline(request, handlerTimeout);
        engine::current_task::SetDeadline(finalDeadline);

        if (request.GetMethod() == kPost) {
            dto::CreateWebshotRequest req;
            try {
                const auto body = json::FromString(request.RequestBody());
                req = body.As<dto::CreateWebshotRequest>();
            } catch (const std::exception &e) {
                return httpu::respondError(response, kBadRequest, "invalid request body");
            }
            try {
                auto parsed = Link::fromUserInput(req.link, config.queryPartLengthMax());
                std::string host = parsed.host();
                if (HostPolicy::IsBareName(host) || HostPolicy::IsDeniedHostname(host) ||
                    HostPolicy::HasSpecialTldSuffix(host))
                    throw InvalidLinkException("forbidden host");
                auto pubs = HostPolicy::resolvePublic(resolver, host, finalDeadline);
                if (pubs.empty())
                    throw InvalidLinkException("forbidden host");
                if (!denylist.isAllowedHost(host))
                    return httpu::respondError(
                        response, kForbidden, "POST failed due to host in denylist"
                    );
                crud.createWebshot(std::move(parsed), std::move(pubs));
                response.SetStatus(kCreated);
                return {};
            } catch (const InvalidLinkException &e) {
                return httpu::respondError(response, kBadRequest, e.what());
            }
        }

        const std::string linkArg = request.GetArg("link");
        if (linkArg.empty())
            return httpu::respondError(response, kBadRequest, "missing parameter: link");
        Link link;
        try {
            link = Link::fromUserInput(linkArg, config.queryPartLengthMax());
        } catch (const InvalidLinkException &e) {
            return httpu::respondError(response, kBadRequest, e.what());
        }
        const auto token = request.GetArg("page_token");
        try {
            auto page = crud.findWebshotByLinkPage(
                link, token.empty() ? std::nullopt : std::make_optional(token)
            );
            return httpu::respondJson(response, kOk, page);
        } catch (const errors::InvalidPageTokenException &) {
            return httpu::respondError(response, kBadRequest, "invalid page_token");
        }
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("Unhandled error in webshot_handler: {}", e.what());
        return httpu::respondError(response, kInternalServerError, "internal server error");
    }
}
