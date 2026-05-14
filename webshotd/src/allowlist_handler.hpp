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

class [[nodiscard]] AllowlistCheckHandler final : public RatelimitedDeadlinedHttpHandler {
public:
    static constexpr std::string_view kName = "allowlist_check";

    explicit AllowlistCheckHandler(
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

class [[nodiscard]] AllowlistAddHandler final : public RatelimitedDeadlinedHttpHandler {
public:
    static constexpr std::string_view kName = "allowlist_add";

    explicit AllowlistAddHandler(
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

class [[nodiscard]] AllowlistRemoveHandler final : public RatelimitedDeadlinedHttpHandler {
public:
    static constexpr std::string_view kName = "allowlist_remove";

    explicit AllowlistRemoveHandler(
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
