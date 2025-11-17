#include "include/webshot_disallow_and_purge_handler.hpp"
/**
 * @file
 * @brief Handler that disallows a domain and enqueues purge of its captures.
 */
#include "include/http_utils.hpp"
#include "include/link.hpp"
#include "include/webshot_config.hpp"
#include "include/webshot_crud.hpp"

#include <string>

#include <fmt/format.h>

#include <userver/components/component.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>

using namespace v1;

WebshotDisallowAndPurgeHandler::WebshotDisallowAndPurgeHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<WebshotCrud>()),
      config(context.FindComponent<WebshotConfig>())
{
}

std::string WebshotDisallowAndPurgeHandler::
    HandleRequestThrow(const server::http::HttpRequest &request, server::request::RequestContext &)
        const
{
    using server::http::HttpStatus::kAccepted;
    using server::http::HttpStatus::kBadRequest;
    using server::http::HttpStatus::kInternalServerError;

    auto &response = request.GetHttpResponse();
    const std::string domain = request.GetArg("domain");
    if (domain.empty())
        return httpu::respondError(response, kBadRequest, "missing parameter: domain");
    Link link;
    try {
        link = Link::fromUserInput(domain, config.queryPartLengthMax());
    } catch (const InvalidLinkException &e) {
        LOG_INFO() << fmt::format("invalid domain: {}", e.what());
        return httpu::respondError(response, kBadRequest, "invalid domain");
    }
    LOG_INFO() << fmt::format("invoked for: {}", link.host());
    try {
        crud.disallowAndPurgeDomain(link.host());
        response.SetStatus(kAccepted);
        return {};
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("failed for {}: {}", link.host(), e.what());
        return httpu::respondError(response, kInternalServerError, "internal server error");
    }
}
