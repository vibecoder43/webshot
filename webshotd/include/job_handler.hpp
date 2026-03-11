#pragma once

#include "integers.hpp"

#include <string_view>

#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/yaml_config/schema.hpp>

namespace us = userver;
namespace server = us::server;

namespace v1 {
class Crud;

/**
 * @brief HTTP handler for polling crawl job status by UUID.
 */
class [[nodiscard]] JobHandler : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "job_handler";
    explicit JobHandler(
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
    const i64 requestTimeoutMs;
};
}; // namespace v1
