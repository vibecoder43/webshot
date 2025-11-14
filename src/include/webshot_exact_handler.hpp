#pragma once
#include "webshot_config.hpp"
#include "webshot_crud.hpp"

#include <string_view>

#include <userver/server/handlers/http_handler_base.hpp>

namespace us = userver;
namespace server = us::server;

namespace v1 {
class [[nodiscard]] WebshotExactHandler : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "webshot-exact";
    explicit WebshotExactHandler(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    [[nodiscard]]
    std::string
    HandleRequestThrow(const server::http::HttpRequest &request, server::request::RequestContext &)
        const final;

private:
    WebshotCrud &crud;
    const WebshotConfig &cfg;
};
} // namespace v1
