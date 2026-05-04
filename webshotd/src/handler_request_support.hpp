#pragma once

#include "client_ip.hpp"
#include "config.hpp"
#include "crud.hpp"
#include "deadline_utils.hpp"
#include "http_utils.hpp"
#include "invariant.hpp"
#include "json.hpp"
#include "link.hpp"
#include "schema/common/common.hpp"
#include "text.hpp"
#include "try.hpp"
#include "uuid_utils.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include <userver/engine/task/current_task.hpp>
#include <userver/http/common_headers.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/utils/assert.hpp>

namespace ws {

namespace us = userver;
namespace server = us::server;
namespace eng = us::engine;
using text::ToBytes;

struct [[nodiscard]] ParamError final {
    String name;
    String message;
};

template <typename T>
[[nodiscard]] Expected<T, String> ParseJsonBodyRequest(const server::http::HttpRequest &request)
{
    using namespace text::literals;

    const auto body = TRY_MAP_ERR(String::FromBytes(request.RequestBody()), [](const auto &) {
        return "invalid request body"_t;
    });
    return ws::json::Parse<T>(body, "invalid request body"_t);
}

[[nodiscard]] inline Expected<Link, String>
ParseJsonLinkBody(const server::http::HttpRequest &request, const Config &config)
{
    using namespace text::literals;

    const auto body = TRY(ParseJsonBodyRequest<::dto::LinkRequest>(request));
    const auto text = TRY_ERR_AS(String::FromBytes(body.link), "invalid parameter"_t);
    return TRY_ERR_AS(Link::FromText(text, config.UrlBytesMax()), "invalid parameter"_t);
}

enum class ClientRequestError {
    kInvalidClientIp,
    kCrudFailure,
};

class [[nodiscard]] HandlerRequestSupport final {
public:
    HandlerRequestSupport(Crud &crud, const Config &config) : crud_(crud), config_(config) {}

    void ApplyRequestDeadline(
        const server::http::HttpRequest &request, std::chrono::milliseconds request_timeout
    ) const
    {
        auto final_deadline = ComputeHandlerDeadline(request, request_timeout);
        eng::current_task::SetDeadline(final_deadline);
    }

    [[nodiscard]] Expected<String, ParamError>
    ParseRequiredQueryText(const server::http::HttpRequest &request, String param_name) const
    {
        const std::string arg = request.GetArg(ToBytes(param_name));
        ENSURE(!arg.empty(), MissingParamError(param_name));
        return TRY_MAP_ERR(String::FromBytes(arg), ([&](auto) {
                               return InvalidParamError(param_name);
                           }));
    }

    [[nodiscard]] Expected<String, ParamError>
    ParseQueryText(const server::http::HttpRequest &request, String param_name) const
    {
        const std::string arg = request.GetArg(ToBytes(param_name));
        return TRY_MAP_ERR(String::FromBytes(arg), ([&](auto) {
                               return InvalidParamError(param_name);
                           }));
    }

    [[nodiscard]] Expected<Link, ParamError>
    ParseRequiredQueryLink(const server::http::HttpRequest &request, String param_name) const
    {
        const auto text = TRY(ParseRequiredQueryText(request, param_name));
        return ParseLinkText(text, param_name);
    }

    template <typename T>
    [[nodiscard]] Expected<T, String> ParseJsonBody(const server::http::HttpRequest &request) const
    {
        return ParseJsonBodyRequest<T>(request);
    }

    [[nodiscard]] Expected<Link, ParamError>
    ParseLinkText(const String &text, String param_name) const
    {
        return TRY_MAP_ERR(Link::FromText(text, config_.UrlBytesMax()), ([&](auto) {
                               return InvalidParamError(param_name);
                           }));
    }

    [[nodiscard]] Expected<Link, ParamError>
    ParseLinkBytes(std::string_view bytes, String param_name) const
    {
        const auto text = TRY_MAP_ERR(String::FromBytes(bytes), ([&](auto) {
                                          return InvalidParamError(param_name);
                                      }));
        return ParseLinkText(text, param_name);
    }

    [[nodiscard]] Expected<ws::uuid::Uuid, ParamError>
    ParseUuidPathArg(const server::http::HttpRequest &request, String param_name) const
    {
        const auto text = TRY(ParseRequiredPathText(request, param_name));
        return TRY_OK_OR(ws::uuid::Parse(text.View()), InvalidParamError(param_name));
    }

    [[nodiscard]] Expected<std::optional<ClientIpCooldown>, ClientRequestError>
    CheckClientIpCooldown(const server::http::HttpRequest &request) const
    {
        auto client_ip = client::ip::Resolve(request, config_);
        if (!client_ip)
            return Unex(ClientRequestError::kInvalidClientIp);

        auto cooldown = crud_.AcquireClientIpCooldown(std::move(*client_ip));
        if (!cooldown)
            return Unex(ClientRequestError::kCrudFailure);
        return *cooldown;
    }

    [[nodiscard]] std::optional<String> RequestHost(const server::http::HttpRequest &request) const
    {
        auto host = String::FromBytes(request.GetHeader(us::http::headers::kHost));
        if (!host)
            return {};
        return *host;
    }

private:
    [[nodiscard]] static ParamError MissingParamError(String param_name)
    {
        using namespace text::literals;

        return {param_name, "missing parameter"_t};
    }

    [[nodiscard]] static ParamError InvalidParamError(String param_name)
    {
        using namespace text::literals;

        return {param_name, "invalid parameter"_t};
    }

    [[nodiscard]] Expected<String, ParamError>
    ParseRequiredPathText(const server::http::HttpRequest &request, String param_name) const
    {
        const std::string arg = request.GetPathArg(ToBytes(param_name));
        ENSURE(!arg.empty(), MissingParamError(param_name));
        return TRY_MAP_ERR(String::FromBytes(arg), ([&](auto) {
                               return InvalidParamError(param_name);
                           }));
    }

    Crud &crud_;
    const Config &config_;
};

[[nodiscard]] inline std::string
RespondClientRequestError(server::http::HttpResponse &response, ClientRequestError error)
{
    using enum server::http::HttpStatus;
    using namespace text::literals;
    using enum ClientRequestError;

    switch (error) {
    case kInvalidClientIp:
        return httpu::RespondError(response, kBadRequest, "invalid client ip"_t);
    case kCrudFailure:
        return httpu::RespondError(response, kInternalServerError, "internal server error"_t);
    default:
        Invariant(""_t);
    }
}

} // namespace ws
