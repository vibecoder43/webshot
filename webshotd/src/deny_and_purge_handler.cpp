#include "deny_and_purge_handler.hpp"
/**
 * @file
 * @brief Handler that denies a host and enqueues purge of its captures.
 */
#include "config.hpp"
#include "crud.hpp"
#include "handler_request_support.hpp"
#include "http.hpp"
#include "integers.hpp"
#include "link.hpp"
#include "prefix_utils.hpp"
#include "text.hpp"

#include <format>
#include <optional>
#include <string>
#include <utility>

#include <userver/components/component.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/utils/assert.hpp>

namespace ws {
namespace us = userver;
namespace server = us::server;
} // namespace ws

using namespace ws;
using namespace text::literals;

DenyPrefixAndPurgeHandler::DenyPrefixAndPurgeHandler(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : RatelimitedDeadlinedHttpHandler(config, context)
{
}

std::string DenyPrefixAndPurgeHandler::HandleRequestThrowRatelimitedDeadlined(
    const server::http::HttpRequest &request, server::request::RequestContext &
) const
{
    using enum server::http::HttpStatus;

    auto &response = request.GetHttpResponse();
    HandlerRequestSupport request_support{config_};

    const auto link = ParseJsonLinkBody(request, config_);
    if (!link)
        return httpu::RespondError(response, kBadRequest, link.Error());

    auto prefix_key = prefix::MakePrefixKey(*link);
    LOG_INFO() << std::format("invoked for prefix: {}", prefix_key);

    auto ok = crud_.DenyPrefixAndPurge(prefix_key);
    if (!ok) {
        LOG_ERROR() << std::format("deny_and_purge failed for prefix {}", prefix_key);
        return httpu::RespondError(response, kInternalServerError, "internal server error"_t);
    }
    response.SetStatus(kAccepted);
    return {};
}
