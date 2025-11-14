#include "include/webshot_handler.hpp"
#include "include/url_validation.hpp"
#include "include/webshot_config.hpp"
#include "schemas/webshot.hpp"

#include <string>

#include <fmt/format.h>

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

using namespace v1;
namespace json = userver::formats::json;

WebshotHandler::WebshotHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<WebshotCrud>()),
      config(context.FindComponent<WebshotConfig>())
{
}

std::string WebshotHandler::
    HandleRequestThrow(const server::http::HttpRequest &request, server::request::RequestContext &)
        const
{
    using server::http::HttpStatus::kBadRequest;
    using server::http::HttpStatus::kCreated;
    using server::http::HttpStatus::kOk;

    const auto normalize = [this](const std::string &s) {
        return tryNormalizeLink(s, config.queryPartLengthMax());
    };

    auto &response = request.GetHttpResponse();
    if (request.GetMethod() == server::http::HttpMethod::kPost) {
        dto::CreateWebshotRequest req;
        try {
            const auto body = json::FromString(request.RequestBody());
            req = body.As<dto::CreateWebshotRequest>();
        } catch (const std::exception &e) {
            response.SetStatus(kBadRequest);
            return {};
        }
        try {
            auto link = normalize(req.link);
            crud.createWebshot(std::move(link));
            response.SetStatus(kCreated);
            return {};
        } catch (const InvalidLinkException &e) {
            response.SetStatus(kBadRequest);
            response.SetContentType(us::http::content_type::kTextPlain);
            return e.what();
        }
    }

    const std::string urlArg = request.GetArg("url");
    if (urlArg.empty()) {
        response.SetStatus(kBadRequest);
        return {};
    }
    std::string normalized;
    try {
        normalized = normalize(urlArg);
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
        LOG_INFO() << "Bad pagination request: " << exc.what();
        response.SetStatus(kBadRequest);
        return exc.what();
    }
}
