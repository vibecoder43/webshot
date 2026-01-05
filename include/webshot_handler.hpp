#pragma once

#include <cstdint>
#include <string_view>

#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace us = userver;
namespace server = us::server;

namespace v1 {
class WebshotConfig;
class WebshotDenylist;
class WebshotCrud;

/**
 * @brief HTTP handler for creating and listing captures for an exact URL.
 *
 * Supports:
 * - POST to enqueue a capture job for the provided link.
 * - GET to list captures for the exact normalized `link` query parameter.
 */
class [[nodiscard]] WebshotHandler : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "webshot-handler";
    explicit WebshotHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

    [[nodiscard]]
    std::string HandleRequestThrow(
        const server::http::HttpRequest &request, server::request::RequestContext &
    ) const final;

private:
    WebshotCrud &crud;
    const WebshotConfig &config;
    WebshotDenylist &denylist;
    const int64_t requestTimeoutMs;
};
}; // namespace v1
