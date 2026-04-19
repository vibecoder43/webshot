#pragma once

#include "integers.hpp"
#include "userver_namespaces.hpp"

#include <chrono>
#include <string>
#include <string_view>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace v1 {
class Crud;
class Config;

/**
 * @brief HTTP handler that lists captures for links sharing a normalized
 * prefix.
 */
class [[nodiscard]] ByPrefixHandler : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "by_prefix";
    explicit ByPrefixHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

    [[nodiscard]]
    std::string HandleRequestThrow(
        const server::http::HttpRequest &request, server::request::RequestContext &
    ) const final;

private:
    Crud &crud;
    const Config &cfg;
    const std::chrono::milliseconds requestTimeout;
};
} // namespace v1
