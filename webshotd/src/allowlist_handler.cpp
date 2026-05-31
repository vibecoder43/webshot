#include "allowlist_handler.hpp"
/**
 * @file
 * @brief Internal endpoints for checking and editing the allowlist.
 */
#include "access_policy.hpp"
#include "config.hpp"
#include "crud.hpp"
#include "handler_request_support.hpp"
#include "http.hpp"
#include "metrics.hpp"
#include "prefix_utils.hpp"
#include "text.hpp"

#include <format>
#include <string>

#include <userver/components/component.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/http/http_method.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>

namespace ws {
namespace us = userver;
namespace server = us::server;
using namespace text::literals;

AllowlistCheckHandler::AllowlistCheckHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : DeadlinedHttpHandler(config, context), config_(context.FindComponent<Config>()),
      access_policy_(context.FindComponent<AccessPolicyStore>()),
      metrics_(context.FindComponent<Metrics>())
{
}

std::string AllowlistCheckHandler::HandleRequestThrowDeadlined(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();

    auto link = ParseJsonLinkBody(request, config_);
    if (!link)
        return httpu::RespondError(response, kBadRequest, link.Error());

    auto allowed = access_policy_.IsAllowlistedPrefix(prefix::MakePrefixKey(*link));
    if (!allowed) {
        metrics_.AccountError(Metrics::Error::kAccessPolicyCheck);
        return httpu::RespondError(response, kInternalServerError, "internal server error"_t);
    }
    if (!*allowed) {
        response.SetStatus(kForbidden);
        return {};
    }

    response.SetStatus(kNoContent);
    return {};
}

AllowlistAddHandler::AllowlistAddHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : DeadlinedHttpHandler(config, context), config_(context.FindComponent<Config>()),
      access_policy_(context.FindComponent<AccessPolicyStore>()),
      metrics_(context.FindComponent<Metrics>())
{
}

std::string AllowlistAddHandler::HandleRequestThrowDeadlined(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();

    auto link = ParseJsonLinkBody(request, config_);
    if (!link)
        return httpu::RespondError(response, kBadRequest, link.Error());

    auto prefix_key = prefix::MakePrefixKey(*link);
    auto inserted = access_policy_.InsertAllowlistPrefix(prefix_key, "allowlist_add"_t);
    if (!inserted) {
        metrics_.AccountError(Metrics::Error::kAccessPolicyCheck);
        LOG_ERROR() << std::format("allowlist add failed for {}", prefix_key);
        return httpu::RespondError(response, kInternalServerError, "internal server error"_t);
    }

    response.SetStatus(kNoContent);
    return {};
}

AllowlistRemoveHandler::AllowlistRemoveHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : DeadlinedHttpHandler(config, context), config_(context.FindComponent<Config>()),
      access_policy_(context.FindComponent<AccessPolicyStore>()),
      metrics_(context.FindComponent<Metrics>())
{
}

std::string AllowlistRemoveHandler::HandleRequestThrowDeadlined(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    auto link = ParseJsonLinkBody(request, config_);
    if (!link)
        return httpu::RespondError(response, kBadRequest, link.Error());

    auto prefix_key = prefix::MakePrefixKey(*link);
    auto removed = access_policy_.RemoveAllowlistPrefix(prefix_key);
    if (!removed) {
        metrics_.AccountError(Metrics::Error::kAccessPolicyCheck);
        LOG_ERROR() << std::format("allowlist remove failed for {}", prefix_key);
        return httpu::RespondError(response, kInternalServerError, "internal server error"_t);
    }
    response.SetStatus(kNoContent);
    return {};
}

} // namespace ws
