#include "include/webshot_by_id_handler.hpp"
/**
 * @file
 * @brief Handler that resolves a capture id to its public location via 302.
 */
#include "include/http_utils.hpp"

#include <fmt/format.h>

#include <userver/components/component.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/http/content_type.hpp>
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
    using server::http::HttpStatus::kBadRequest;
    using server::http::HttpStatus::kFound;
    using server::http::HttpStatus::kInternalServerError;
    using server::http::HttpStatus::kNotFound;
    using us::http::content_type::kApplicationJson;

    auto &response = request.GetHttpResponse();
    try {
        const std::string uuidStr = request.GetPathArg("uuid");
        if (uuidStr.empty())
            return httpu::respondError(response, kBadRequest, "missing parameter: uuid");
        const auto uuid = us::utils::BoostUuidFromString(uuidStr);
        auto webshot = crud.findWebshot(uuid);
        if (!webshot) {
            LOG_INFO() << fmt::format("Webshot not found: {}", us::utils::ToString(uuid));
            return httpu::respondError(response, kNotFound, "webshot not found");
        }
        response.SetStatus(kFound);
        response.SetHeader(us::http::headers::kLocation, webshot->location);
        return {};
    } catch (const std::exception &e) {
        LOG_ERROR() << fmt::format("Unhandled error in webshot_by_id: {}", e.what());
        return httpu::respondError(response, kInternalServerError, "internal server error");
    }
}
