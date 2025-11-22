#pragma once

#include <cstdint>
#include <string_view>

#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace us = userver;
namespace server = us::server;

namespace v1 {
class WebshotCrud;
/**
 * @brief HTTP handler that redirects to the stored capture by id.
 */
class [[nodiscard]] WebshotById : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "webshot-by-id";
    explicit WebshotById(
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
    const int64_t requestTimeoutMs;
};
}; // namespace v1
