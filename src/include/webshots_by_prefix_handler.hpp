#pragma once
#include <string_view>

#include <userver/server/handlers/http_handler_base.hpp>

namespace us = userver;
namespace server = us::server;

namespace v1 {
class WebshotCrud;
class WebshotConfig;

/**
 * @brief HTTP handler that lists captures for links sharing a normalized
 * prefix.
 */
class [[nodiscard]] WebshotsByPrefixHandler : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "webshots-by-prefix";
    explicit WebshotsByPrefixHandler(
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
