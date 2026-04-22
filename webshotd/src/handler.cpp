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
#include "handler_request_support.hpp"
#include "http_utils.hpp"
#include "integers.hpp"
#include "metrics.hpp"
#include "prefix_utils.hpp"
#include "schema/webshot.hpp"
#include "server_errors.hpp"
#include "text.hpp"

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
    HandlerRequestSupport requestSupport{crud, config};
    requestSupport.applyRequestDeadline(request, requestTimeout);

    if (request.GetMethod() == kPost) {
        const auto req = requestSupport.parseJsonBody<dto::CreateCaptureRequest>(request);
        if (!req)
            return httpu::respondError(response, kBadRequest, req.error());

        auto parsed = requestSupport.parseLinkBytes(req->link, "link"_t);
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

        const auto cooldown = requestSupport.checkClientIpCooldown(request);
        if (!cooldown)
            return respondClientRequestError(response, cooldown.error());
        if (*cooldown)
            return httpu::respondClientIpCooldown(response, (*cooldown)->retryAfter);

        auto job = crud.createCaptureJob(std::move(*parsed));
        if (!job)
            return httpu::respondError(response, kInternalServerError, "internal server error"_t);
        return httpu::respondJson(response, kAccepted, *job);
    }

    const auto link = requestSupport.parseRequiredQueryLink(request, "link"_t);
    if (!link)
        return httpu::respondParamError(
            response, kBadRequest, link.error().name, link.error().message
        );

    const auto token = requestSupport.parseQueryText(request, "page_token"_t);
    if (!token)
        return httpu::respondParamError(
            response, kBadRequest, token.error().name, token.error().message
        );

    const auto cooldown = requestSupport.checkClientIpCooldown(request);
    if (!cooldown)
        return respondClientRequestError(response, cooldown.error());
    if (*cooldown)
        return httpu::respondClientIpCooldown(response, (*cooldown)->retryAfter);

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
