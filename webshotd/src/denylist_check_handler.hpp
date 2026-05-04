#pragma once

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
class Config;
class Denylist;
class Metrics;
class Crud;

class [[nodiscard]] DenylistCheckHandler : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "denylist_check";

    explicit DenylistCheckHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

    [[nodiscard]]
    std::string HandleRequestThrow(
        const server::http::HttpRequest &request, server::request::RequestContext &
    ) const final;

private:
    const Config &config_;
    Denylist &denylist_;
    Metrics &metrics_;
    Crud &crud_;
    const std::chrono::milliseconds request_timeout;
};

} // namespace ws
