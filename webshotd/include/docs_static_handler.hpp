#pragma once

#include <string>
#include <string_view>

#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace us = userver;
namespace server = us::server;

namespace v1 {

class [[nodiscard]] ScalarAssetsHandler final : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "scalar_assets";

    explicit ScalarAssetsHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

    [[nodiscard]]
    std::string HandleRequestThrow(
        const server::http::HttpRequest &request, server::request::RequestContext &
    ) const final;

private:
    std::string rootDir;
};

class [[nodiscard]] OpenApiHandler final : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "openapi_static";

    explicit OpenApiHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

    [[nodiscard]]
    std::string HandleRequestThrow(
        const server::http::HttpRequest &request, server::request::RequestContext &
    ) const final;

private:
    std::string rootDir;
};

} // namespace v1
