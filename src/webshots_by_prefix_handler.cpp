#include "include/webshots_by_prefix_handler.hpp"

#include "schemas/webshot.hpp"

#include <string>

#include <userver/components/component.hpp>
#include <userver/formats/json.hpp>
#include <userver/http/content_type.hpp>
#include <userver/server/http/http_method.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>

namespace us = userver;
namespace json = userver::formats::json;

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
    auto &response = request.GetHttpResponse();
    if (request.GetMethod() != server::http::HttpMethod::kGet) {
        response.SetStatus(server::http::HttpStatus::kMethodNotAllowed);
        return {};
    }

    const std::string prefixArg = request.GetArg("prefix");
    if (prefixArg.empty()) {
        response.SetStatus(server::http::HttpStatus::kBadRequest);
        return {};
    }
    std::string normalizedPrefix;
    try {
        // Use the same normalization as exact search; returns scheme-less link string
        normalizedPrefix = tryNormalizeLink(prefixArg, cfg.queryPartLengthMax());
    } catch (const InvalidLinkException &e) {
        response.SetStatus(server::http::HttpStatus::kBadRequest);
        response.SetContentType(us::http::content_type::kTextPlain);
        return e.what();
    }
    const auto token = request.GetArg("page_token");

    auto page = crud.findWebshotsByPrefixPage(
        normalizedPrefix, token.empty() ? std::nullopt : std::optional<std::string>(token)
    );
    response.SetStatus(server::http::HttpStatus::kOk);
    response.SetContentType(us::http::content_type::kApplicationJson);
    return json::ToString(json::ValueBuilder(page).ExtractValue());
}
