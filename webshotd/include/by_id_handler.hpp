#pragma once

#include "integers.hpp"
#include "userver_namespaces.hpp"

#include <string>
#include <string_view>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace v1 {
class Config;
class Crud;
/**
 * @brief HTTP handler that returns stored capture metadata by id.
 */
class [[nodiscard]] ById : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "by_id";
    explicit ById(
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
    const i64 requestTimeoutMs;
};
} // namespace v1
