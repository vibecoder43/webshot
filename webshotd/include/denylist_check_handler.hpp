#pragma once

#include "integers.hpp"

#include <string_view>

#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace us = userver;
namespace server = us::server;

namespace v1 {

class Config;
class Denylist;

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
    const Config &config;
    Denylist &denylist;
    const i64 requestTimeoutMs;
};

} // namespace v1
