#include "include/webshots_by_prefix_handler.hpp"
/**
 * @file
 * @brief Handler that lists captures grouped by normalized link prefix.
 */
#include "include/link.hpp"

#include <string>

#include "include/http_utils.hpp"
#include "include/server_errors.hpp"
#include <userver/components/component.hpp>
#include <userver/formats/json.hpp>
#include <userver/http/content_type.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/http/http_method.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>

namespace us = userver;

using namespace v1;

WebshotsByPrefixHandler::WebshotsByPrefixHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<WebshotCrud>()),
      cfg(context.FindComponent<WebshotConfig>())
{
}

std::string WebshotsByPrefixHandler::
    HandleRequestThrow(const server::http::HttpRequest &request, server::request::RequestContext &)
        const
{
    using server::http::HttpStatus::kBadRequest;
    using server::http::HttpStatus::kInternalServerError;
    using server::http::HttpStatus::kMethodNotAllowed;
    using server::http::HttpStatus::kOk;
    using us::http::content_type::kApplicationJson;
    auto &response = request.GetHttpResponse();
    try {
        const std::string prefixArg = request.GetArg("prefix");
        if (prefixArg.empty())
            return httpu::respondError(response, kBadRequest, "missing parameter: prefix");
        std::string normalizedPrefix;
        try {
            normalizedPrefix =
                Link::fromUserInput(prefixArg, cfg.queryPartLengthMax()).normalized();
        } catch (const InvalidLinkException &e) {
            return httpu::respondError(response, kBadRequest, e.what());
        }
        const auto token = request.GetArg("page_token");

        try {
            auto page = crud.findWebshotsByPrefixPage(
                normalizedPrefix, token.empty() ? std::nullopt : std::optional<std::string>(token)
            );
            return httpu::respondJson(response, kOk, page);
        } catch (const errors::InvalidPageTokenException &) {
            return httpu::respondError(response, kBadRequest, "invalid page_token");
        }
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("Unhandled error: ", e.what());
        return httpu::respondError(response, kInternalServerError, "internal server error");
    }
}
