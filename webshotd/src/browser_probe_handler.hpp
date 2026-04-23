#pragma once

#include "integers.hpp"
#include "userver_namespaces.hpp"

#include <memory>
#include <string>
#include <string_view>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace v1 {

class Config;

class [[nodiscard]] BrowserProbeHandler final : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "browser_probe";

    explicit BrowserProbeHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );
    ~BrowserProbeHandler() override;

    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

    [[nodiscard]]
    std::string HandleRequestThrow(
        const server::http::HttpRequest &request, server::request::RequestContext &
    ) const final;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace v1
