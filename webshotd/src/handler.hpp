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
class Config;
class Denylist;
class Crud;
class Metrics;

/**
 * @brief HTTP handler for creating and listing captures for an exact URL.
 *
 * Supports:
 * - POST to enqueue a capture job for the provided link.
 * - GET to list captures for the exact normalized `link` query parameter.
 */
class [[nodiscard]] Handler : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "handler";
    explicit Handler(
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
    const Config &config;
    Denylist &denylist;
    Metrics &metrics;
    const std::chrono::milliseconds requestTimeout;
};
} // namespace v1
