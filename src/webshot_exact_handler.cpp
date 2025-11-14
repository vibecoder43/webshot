#include "include/webshot_exact_handler.hpp"

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

WebshotExactHandler::WebshotExactHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<WebshotCrud>()),
      cfg(context.FindComponent<WebshotConfig>())
{
}

std::string WebshotExactHandler::
    HandleRequestThrow(const server::http::HttpRequest &request, server::request::RequestContext &)
        const
{
    using server::http::HttpStatus::kBadRequest;
    using server::http::HttpStatus::kOk;

    auto &response = request.GetHttpResponse();
    if (request.GetMethod() != server::http::HttpMethod::kGet) {
        response.SetStatus(server::http::HttpStatus::kMethodNotAllowed);
        return {};
    }

    const std::string linkArg = request.GetArg("link");
    if (linkArg.empty()) {
        response.SetStatus(kBadRequest);
        return {};
    }
    std::string normalized;
    try {
        normalized = tryNormalizeLink(linkArg, cfg.queryPartLengthMax());
    } catch (const InvalidLinkException &e) {
        response.SetStatus(kBadRequest);
        response.SetContentType(us::http::content_type::kTextPlain);
        return e.what();
    }
    const auto token = request.GetArg("page_token");
    try {
        auto page = crud.findWebshotByLinkPage(
            normalized, token.empty() ? std::nullopt : std::make_optional(token)
        );
        response.SetStatus(kOk);
        response.SetContentType(us::http::content_type::kApplicationJson);
        return json::ToString(json::ValueBuilder(page).ExtractValue());
    } catch (const std::exception &exc) {
        response.SetStatus(kBadRequest);
        return exc.what();
    }
}
