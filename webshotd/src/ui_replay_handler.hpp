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

class [[nodiscard]] UiReplayHandler final : public DeadlinedHttpHandler {
public:
    static constexpr std::string_view kName = "ui_replay";

    explicit UiReplayHandler(
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
