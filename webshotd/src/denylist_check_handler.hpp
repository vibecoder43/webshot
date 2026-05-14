#pragma once

#include "http.hpp"

#include <string>
#include <string_view>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>

namespace ws {

namespace us = userver;
namespace server = us::server;
class Config;
class AccessPolicyStore;
class Metrics;

class [[nodiscard]] AccessPolicyCheckHandler : public RatelimitedDeadlinedHttpHandler {
public:
    static constexpr std::string_view kName = "denylist_check";

    explicit AccessPolicyCheckHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]]
    std::string HandleRequestThrowRatelimitedDeadlined(
        const server::http::HttpRequest &request, server::request::RequestContext &
    ) const final;

private:
    AccessPolicyStore &access_policy_;
    Metrics &metrics_;
};

} // namespace ws
