#include "handler.hpp"
/**
 * @file
 * @brief Handler that creates captures and lists them by exact link.
 */
#include "access_policy.hpp"
#include "config.hpp"
#include "crud.hpp"
#include "handler_request_support.hpp"
#include "http.hpp"
#include "metrics.hpp"
#include "prefix_utils.hpp"
#include "schema/public/webshot.hpp"
#include "server_errors.hpp"
#include "text.hpp"

#include <string>
#include <utility>

#include <userver/components/component.hpp>
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

namespace ws {
namespace us = userver;
namespace server = us::server;
} // namespace ws

using namespace ws;
using namespace text::literals;

CaptureByLinkHandler::CaptureByLinkHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : RatelimitedDeadlinedHttpHandler(config, context),
      access_policy_(context.FindComponent<AccessPolicyStore>()),
      metrics_(context.FindComponent<Metrics>())
{
}

std::string CaptureByLinkHandler::HandleRequestThrowRatelimitedDeadlined(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using server::http::HttpMethod::kPost;
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    HandlerRequestSupport request_support{config_};

    if (request.GetMethod() == kPost) {
        const auto req = request_support.ParseJsonBody<dto::CreateCaptureRequest>(request);
        if (!req)
            return httpu::RespondError(response, kBadRequest, req.Error());

        auto parsed = request_support.ParseLinkBytes(req->link, "link"_t);
        if (!parsed)
            return httpu::RespondError(response, kBadRequest, "invalid parameter"_t);
        auto prefix_key = prefix::MakePrefixKey(*parsed);
        const auto decision = access_policy_.EvaluatePrefix(
            prefix_key,
            config_.AllowlistOnly() ? AccessPolicyMode::kAllowlistOnly : AccessPolicyMode::kRegular
        );
        if (!decision) {
            metrics_.AccountError(Metrics::Error::kAccessPolicyCheck);
            return httpu::RespondError(response, kInternalServerError, "internal server error"_t);
        }
        if (!decision->allowed)
            return httpu::RespondError(
                response, kForbidden, AccessDecisionMessage(decision->reason)
            );

        auto job = crud_.CreateCaptureJob(std::move(*parsed));
        if (!job)
            return httpu::RespondError(response, kInternalServerError, "internal server error"_t);
        return httpu::RespondJson(response, kAccepted, *job);
    }

    const auto link = request_support.ParseRequiredQueryLink(request, "link"_t);
    if (!link)
        return httpu::RespondParamError(
            response, kBadRequest, link.Error().name, link.Error().message
        );

    const auto token = request_support.ParseQueryText(request, "page_token"_t);
    if (!token)
        return httpu::RespondParamError(
            response, kBadRequest, token.Error().name, token.Error().message
        );

    auto page = crud_.FindCapturesByLinkPage(*link, *token);
    if (!page) {
        using enum errors::CapturePageError;
        if (page.Error() == kDbError)
            return httpu::RespondError(response, kInternalServerError, "internal server error"_t);
        return httpu::RespondParamError(
            response, kBadRequest, "page_token"_t, "invalid page_token"_t
        );
    }
    return httpu::RespondJson(response, kOk, *page);
}
