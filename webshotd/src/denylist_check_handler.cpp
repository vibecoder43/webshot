#include "denylist_check_handler.hpp"
/**
 * @file
 * @brief Internal endpoint for checking whether a link is denylisted.
 */
#include "access_policy.hpp"
#include "config.hpp"
#include "handler_request_support.hpp"
#include "http.hpp"
#include "metrics.hpp"
#include "prefix_utils.hpp"

#include <string>

#include <userver/components/component.hpp>
#include <userver/engine/exception.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/server/http/http_method.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>

namespace ws {
namespace us = userver;
namespace server = us::server;

AccessPolicyCheckHandler::AccessPolicyCheckHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : DeadlinedHttpHandler(config, context), config_(context.FindComponent<Config>()),
      access_policy_(context.FindComponent<AccessPolicyStore>()),
      metrics_(context.FindComponent<Metrics>())
{
}

std::string AccessPolicyCheckHandler::HandleRequestThrowDeadlined(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    const auto link = ParseJsonLinkBody(request, config_);
    if (!link)
        return httpu::RespondError(response, kBadRequest, link.Error());

    auto prefix_key = prefix::MakePrefixKey(*link);
    const auto allowed = access_policy_.IsAllowedPrefix(prefix_key);
    if (!allowed) {
        metrics_.AccountError(Metrics::Error::kAccessPolicyCheck);
        response.SetStatus(kInternalServerError);
        return {};
    }
    if (!*allowed) {
        response.SetStatus(kForbidden);
        return {};
    }
    response.SetStatus(kNoContent);
    return {};
}

} // namespace ws
