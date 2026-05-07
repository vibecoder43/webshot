#pragma once

#include <userver/server/middlewares/http_middleware_base.hpp>

namespace ws {
namespace us = userver;
namespace server = us::server;

class ExceptionHandlingMiddleware final : public server::middlewares::HttpMiddlewareBase {
public:
    static constexpr std::string_view kName = "ws-exception-handling-middleware";

    explicit ExceptionHandlingMiddleware(const server::handlers::HttpHandlerBase &);

private:
    void HandleRequest(
        server::http::HttpRequest &request, server::request::RequestContext &context
    ) const override;
};

} // namespace ws
