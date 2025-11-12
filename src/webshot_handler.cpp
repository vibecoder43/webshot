#include "include/webshot_handler.hpp"
#include "include/url_validation.hpp"
#include "schemas/webshot.hpp"

#include <ada.h>

#include <fmt/format.h>

#include <userver/components/component.hpp>
#include <userver/formats/json.hpp>
#include <userver/formats/serialize/common_containers.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/http/content_type.hpp>
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
    : HttpHandlerBase(config, context), crud(context.FindComponent<WebshotCrud>())
{
}

std::string WebshotHandler::
    HandleRequestThrow(const server::http::HttpRequest &request, server::request::RequestContext &)
        const
{
    auto &response = request.GetHttpResponse();
    if (request.GetMethod() == server::http::HttpMethod::kPost) {
        dto::CreateWebshotRequest req;
        try {
            const auto body = json::FromString(request.RequestBody());
            req = body.As<dto::CreateWebshotRequest>();
        } catch (const std::exception &e) {
            response.SetStatus(server::http::HttpStatus::kBadRequest);
            return {};
        }
        auto url = ada::parse<ada::url_aggregator>(req.url);
        if (!url || !isValidForWebshotUrl(*url)) {
            LOG_INFO() << fmt::format(
                "Invalid url submitted: {}", url ? url->get_href() : "failed to parse"
            );
            response.SetStatus(server::http::HttpStatus::kBadRequest);
            return {};
        }
        crud.createWebshot(urlToLink(*url));
        response.SetStatus(server::http::HttpStatus::kCreated);
        return {};
    }

    const std::string urlArg = request.GetArg("url");
    if (urlArg.empty()) {
        response.SetStatus(server::http::HttpStatus::kBadRequest);
        return {};
    }
    auto url = ada::parse<ada::url_aggregator>(urlArg);
    if (!url || !isValidForWebshotUrl(*url)) {
        LOG_INFO(
        ) << fmt::format("Invalid url for lookup: {}", url ? url->get_href() : "failed to parse");
        response.SetStatus(server::http::HttpStatus::kBadRequest);
        return {};
    }
    const auto normalized = urlToLink(*url);
    auto ret = crud.findWebshotByLink(normalized);
    if (ret.empty()) {
        LOG_INFO() << fmt::format("No webshots for link: {}", normalized);
        response.SetStatus(server::http::HttpStatus::kNotFound);
        return {};
    }
    response.SetStatus(server::http::HttpStatus::kOk);
    response.SetContentType(us::http::content_type::kApplicationJson);
    return json::ToString(json::ValueBuilder(ret).ExtractValue());
}
