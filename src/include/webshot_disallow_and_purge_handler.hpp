#pragma once

#include <string_view>

#include <userver/server/handlers/http_handler_base.hpp>

namespace us = userver;
namespace server = us::server;

namespace v1 {
class WebshotCrud;
class WebshotConfig;

/**
 * @brief HTTP handler that disallows a domain and enqueues purge.
 *
 * Accepts a `domain` query argument, normalizes it via Crud, inserts it into
 * the denylist, and enqueues background purge of matching captures.
 */
class [[nodiscard]] WebshotDisallowAndPurgeHandler : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "webshot-disallow-and-purge";
    explicit WebshotDisallowAndPurgeHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]]
    std::string
    HandleRequestThrow(const server::http::HttpRequest &request, server::request::RequestContext &)
        const final;

private:
    WebshotCrud &crud;
    const WebshotConfig &config;
};
}; // namespace v1
