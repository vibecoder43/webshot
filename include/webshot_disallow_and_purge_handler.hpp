#pragma once

#include <cstdint>
#include <string_view>

#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace us = userver;
namespace server = us::server;

namespace v1 {
class WebshotCrud;
class WebshotConfig;

/**
 * @brief HTTP handler that disallows a host and enqueues purge.
 *
 * Accepts a `host` query argument, normalizes it via Crud, inserts it into
 * the denylist, and enqueues background purge of matching captures.
 */
class [[nodiscard]] WebshotDisallowAndPurgeHandler : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "webshot-disallow-and-purge";
    explicit WebshotDisallowAndPurgeHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

    [[nodiscard]]
    std::string
    HandleRequestThrow(const server::http::HttpRequest &request, server::request::RequestContext &)
        const final;

private:
    WebshotCrud &crud;
    const WebshotConfig &config;
    const int64_t requestTimeoutMs;
};
}; // namespace v1
