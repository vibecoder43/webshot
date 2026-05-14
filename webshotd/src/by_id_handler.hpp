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
 * @brief HTTP handler that returns stored capture metadata by id.
 */
class [[nodiscard]] ByIdHandler : public RatelimitedDeadlinedHttpHandler {
public:
    static constexpr std::string_view kName = "by_id";
    explicit ByIdHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]]
    std::string HandleRequestThrowRatelimitedDeadlined(
        const server::http::HttpRequest &request, server::request::RequestContext &
    ) const final;
};
} // namespace ws
