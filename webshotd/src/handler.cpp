#include "handler.hpp"
/**
 * @file
 * @brief Handler that creates captures and lists them by exact link.
 */
#include "client_ip.hpp"
#include "config.hpp"
#include "crud.hpp"
#include "deadline_utils.hpp"
#include "denylist.hpp"
#include "http_utils.hpp"
#include "integers.hpp"
#include "link.hpp"
#include "metrics.hpp"
#include "prefix_utils.hpp"
#include "schema/webshot.hpp"
#include "server_errors.hpp"
#include "text.hpp"
#include "userver_expected.hpp"

#include <chrono>
#include <string>
#include <utility>

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
using namespace std::chrono_literals;
using namespace text::literals;

Handler::Handler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<Crud>()),
      config(context.FindComponent<Config>()), denylist(context.FindComponent<Denylist>()),
      metrics(context.FindComponent<Metrics>()),
      requestTimeout(config["request-timeout-ms"].As<int64_t>() * 1ms)
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
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    auto finalDeadline = computeHandlerDeadline(request, requestTimeout);
    eng::current_task::SetDeadline(finalDeadline);

    if (request.GetMethod() == kPost) {
        const auto body = String::fromBytes(request.RequestBody());
        if (!body)
            return httpu::respondError(response, kBadRequest, "invalid request body"_t);
        const auto req = exu::json::parse<dto::CreateCaptureRequest>(
            body->view(), "invalid request body"_t
        );
        if (!req) {
            return httpu::respondError(response, kBadRequest, "invalid request body"_t);
        }

        const auto linkText = String::fromBytes(req->link);
        if (!linkText)
            return httpu::respondError(response, kBadRequest, "invalid parameter"_t);
        auto parsed = Link::fromText(*linkText, config.urlBytesMax());
        if (!parsed)
            return httpu::respondError(response, kBadRequest, "invalid parameter"_t);
        auto prefixKey = prefix::makePrefixKey(*parsed);
        const auto allowed = denylist.isAllowedPrefix(prefixKey);
        if (!allowed) {
            metrics.accountError(Metrics::Error::kDenylistCheck);
            return httpu::respondError(response, kInternalServerError, "internal server error"_t);
        }
        if (!*allowed)
            return httpu::respondError(response, kForbidden, "host in denylist"_t);

        auto clientIp = client_ip::resolve(request, config);
        if (!clientIp)
            return httpu::respondError(response, kBadRequest, "invalid client ip"_t);
        auto cooldown = *crud.acquireClientIpCooldown(std::move(*clientIp));
        if (cooldown)
            return httpu::respondClientIpCooldown(response, cooldown->retryAfter);

        auto job = crud.createCaptureJob(std::move(*parsed));
        if (!job)
            return httpu::respondError(response, kInternalServerError, "internal server error"_t);
        return httpu::respondJson(response, kAccepted, *job);
    }

    const std::string arg = request.GetArg("link");
    if (arg.empty())
        return httpu::respondParamError(response, kBadRequest, "link"_t, "missing parameter"_t);
    auto str = String::fromBytes(arg);
    if (!str)
        return httpu::respondParamError(response, kBadRequest, "link"_t, "invalid parameter"_t);
    const auto link = Link::fromText(*str, config.urlBytesMax());
    if (!link)
        return httpu::respondParamError(response, kBadRequest, "link"_t, "invalid parameter"_t);
    const std::string tokenArg = request.GetArg("page_token");
    const auto token = String::fromBytes(tokenArg);
    if (!token)
        return httpu::respondParamError(
            response, kBadRequest, "page_token"_t, "missing parameter"_t
        );
    auto clientIp = client_ip::resolve(request, config);
    if (!clientIp)
        return httpu::respondError(response, kBadRequest, "invalid client ip"_t);
    auto cooldown = *crud.acquireClientIpCooldown(std::move(*clientIp));
    if (cooldown)
        return httpu::respondClientIpCooldown(response, cooldown->retryAfter);

    auto page = crud.findCapturesByLinkPage(*link, *token);
    if (!page) {
        using enum errors::CapturePageError;
        if (page.error() == kDbFailure)
            return httpu::respondError(response, kInternalServerError, "internal server error"_t);
        return httpu::respondParamError(
            response, kBadRequest, "page_token"_t, "invalid page_token"_t
        );
    }
    return httpu::respondJson(response, kOk, *page);
}
