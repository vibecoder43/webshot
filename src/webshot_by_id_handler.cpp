#include "include/webshot_by_id_handler.hpp"

#include <fmt/format.h>

#include <userver/components/component.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/utils/boost_uuid4.hpp>

using namespace v1;

WebshotById::WebshotById(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : HttpHandlerBase(config, context), crud(context.FindComponent<WebshotCrud>())
{
}

std::string WebshotById::
    HandleRequestThrow(const server::http::HttpRequest &request, server::request::RequestContext &)
        const
{
    auto &response = request.GetHttpResponse();
    const std::string uuidStr = request.GetPathArg("uuid");
    if (uuidStr.empty()) {
        response.SetStatus(server::http::HttpStatus::kBadRequest);
        return {};
    }
    const auto uuid = us::utils::BoostUuidFromString(uuidStr);
    auto webshot = crud.findWebshot(uuid);
    if (!webshot) {
        LOG_INFO() << fmt::format("Webshot not found: {}", us::utils::ToString(uuid));
        response.SetStatus(server::http::HttpStatus::kNotFound);
        return {};
    }
    response.SetStatus(server::http::HttpStatus::kFound);
    response.SetHeader(us::http::headers::kLocation, webshot->location);
    return {};
}
