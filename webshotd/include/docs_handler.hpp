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

class [[nodiscard]] DocsHandler final : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "docs";

    explicit DocsHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

    [[nodiscard]]
    std::string HandleRequestThrow(
        const server::http::HttpRequest &request, server::request::RequestContext &
    ) const final;

private:
    const std::chrono::milliseconds requestTimeout;
    const std::string title;
    const std::string specUrl;
};

} // namespace v1
