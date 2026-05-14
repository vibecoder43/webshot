#pragma once

#include "http.hpp"
#include "integers.hpp"

#include <string>
#include <string_view>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>

namespace ws {
namespace us = userver;
namespace server = us::server;
class Crud;
class Config;

/**
 * @brief HTTP handler that denies a link prefix and enqueues purge.
 *
 * Accepts a JSON link request body, derives its prefix, inserts it into
 * the denylist, and enqueues background purge of matching captures.
 */
class [[nodiscard]] DenyPrefixAndPurgeHandler : public DeadlinedHttpHandler {
public:
    static constexpr std::string_view kName = "deny_and_purge";
    explicit DenyPrefixAndPurgeHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]]
    std::string HandleRequestThrowDeadlined(
        const server::http::HttpRequest &request, server::request::RequestContext &
    ) const final;

private:
    Crud &crud_;
    const Config &config_;
};
} // namespace ws
