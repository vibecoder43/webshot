#include "by_prefix_handler.hpp"
/**
 * @file
 * @brief Handler that lists captures grouped by normalized link prefix.
 */
#include "config.hpp"
#include "crud.hpp"
#include "handler_request_support.hpp"
#include "integers.hpp"
#include "link.hpp"
#include "text.hpp"

#include <format>
#include <string>
#include <utility>

#include "http.hpp"
#include "server_errors.hpp"
#include <userver/components/component.hpp>
#include <userver/engine/exception.hpp>
#include <userver/formats/json.hpp>
#include <userver/http/content_type.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/http/http_method.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>

namespace ws {
namespace us = userver;
namespace server = us::server;
} // namespace ws

using namespace ws;
using namespace text::literals;

ByPrefixHandler::ByPrefixHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : DeadlinedHttpHandler(config, context), crud_(context.FindComponent<Crud>()),
      config_(context.FindComponent<Config>())
{
}

std::string ByPrefixHandler::HandleRequestThrowDeadlined(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;
    auto &response = request.GetHttpResponse();
    HandlerRequestSupport request_support{crud_, config_};

    const auto prefix = request_support.ParseRequiredQueryLink(request, "prefix"_t);
    if (!prefix)
        return httpu::RespondParamError(
            response, kBadRequest, prefix.Error().name, prefix.Error().message
        );

    const auto token = request_support.ParseQueryText(request, "page_token"_t);
    if (!token)
        return httpu::RespondParamError(
            response, kBadRequest, token.Error().name, token.Error().message
        );

    const auto cooldown = request_support.CheckClientIpCooldown(request);
    if (!cooldown)
        return RespondClientRequestError(response, cooldown.Error());
    if (*cooldown)
        return httpu::RespondClientIpCooldown(response, (*cooldown)->retry_after);

    auto page = crud_.FindCapturesByPrefixPage(prefix->Normalized(), *token);
    if (!page) {
        using enum errors::CapturePageError;
        if (page.Error() == kDbError)
            return httpu::RespondError(response, kInternalServerError, "internal server error"_t);
        return httpu::RespondParamError(
            response, kBadRequest, "page_token"_t, "invalid page_token"_t
        );
    }
    return httpu::RespondJson(response, kOk, *page);
}
