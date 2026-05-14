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
class Crud;

class [[nodiscard]] AccessPolicyCheckHandler : public DeadlinedHttpHandler {
public:
    static constexpr std::string_view kName = "denylist_check";

    explicit AccessPolicyCheckHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]]
    std::string HandleRequestThrowDeadlined(
        const server::http::HttpRequest &request, server::request::RequestContext &
    ) const final;

private:
    const Config &config_;
    AccessPolicyStore &access_policy_;
    Metrics &metrics_;
    Crud &crud_;
};

} // namespace ws
