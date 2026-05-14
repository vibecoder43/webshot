#pragma once

#include "http.hpp"
#include "integers.hpp"

#include <string>
#include <string_view>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/yaml_config/schema.hpp>

namespace ws {

namespace us = userver;
namespace server = us::server;
class [[nodiscard]] DocsHandler final : public DeadlinedHttpHandler {
public:
    static constexpr std::string_view kName = "docs";

    explicit DocsHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

    [[nodiscard]]
    std::string HandleRequestThrowDeadlined(
        const server::http::HttpRequest &request, server::request::RequestContext &
    ) const final;

private:
    const std::string title;
    const std::string spec_url;
};

} // namespace ws
