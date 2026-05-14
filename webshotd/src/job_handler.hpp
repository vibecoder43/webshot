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
class Config;
class Crud;

/**
 * @brief HTTP handler for polling crawl job status by UUID.
 */
class [[nodiscard]] JobHandler : public RatelimitedDeadlinedHttpHandler {
public:
    static constexpr std::string_view kName = "job_handler";
    explicit JobHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]]
    std::string HandleRequestThrowRatelimitedDeadlined(
        const server::http::HttpRequest &request, server::request::RequestContext &
    ) const final;

protected:
    [[nodiscard]] std::string RespondClientIpRatelimit(
        const server::http::HttpRequest &request, std::chrono::milliseconds retry_after
    ) const override;
};
} // namespace ws
