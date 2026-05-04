#pragma once

#include "integers.hpp"

#include <chrono>
#include <string>
#include <string_view>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace ws {
namespace us = userver;
namespace server = us::server;
class Crud;
class Config;

/**
 * @brief HTTP handler that disallows a host and enqueues purge.
 *
 * Accepts a JSON link request body, normalizes it via Crud, inserts it into
 * the denylist, and enqueues background purge of matching captures.
 */
class [[nodiscard]] DisallowAndPurgeHandler : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "disallow_and_purge";
    explicit DisallowAndPurgeHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

    [[nodiscard]]
    std::string HandleRequestThrow(
        const server::http::HttpRequest &request, server::request::RequestContext &
    ) const final;

private:
    Crud &crud_;
    const Config &config_;
    const std::chrono::milliseconds request_timeout;
};
} // namespace ws
