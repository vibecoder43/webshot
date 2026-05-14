#include "exception_handling_middleware.hpp"

#include "error_utils.hpp"
#include "http.hpp"
#include "text.hpp"

#include <userver/engine/exception.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>

namespace ws {
namespace us = userver;
namespace server = us::server;

ExceptionHandlingMiddleware::ExceptionHandlingMiddleware(const server::handlers::HttpHandlerBase &)
{
}

void ExceptionHandlingMiddleware::HandleRequest(
    server::http::HttpRequest &request, server::request::RequestContext &context
) const
{
    using namespace text::literals;
    using enum server::http::HttpStatus;

    auto set_error = [&](String message) {
        request.GetHttpResponse().SetData(
            httpu::RespondError(request.GetHttpResponse(), kInternalServerError, message)
        );
    };

    try {
        Next(request, context);
    } catch (const us::engine::WaitInterruptedException &) {
        set_error("cancelled"_t);
    } catch (const us::engine::TaskCancelledException &) {
        set_error("cancelled"_t);
    } catch (const std::exception &) {
        set_error("internal server error"_t);
    }
}

} // namespace ws
