#pragma once

#include "client_ip.hpp"
#include "config.hpp"
#include "crud.hpp"
#include "deadline_utils.hpp"
#include "http_utils.hpp"
#include "link.hpp"
#include "text.hpp"
#include "try.hpp"
#include "userver_namespaces.hpp"
#include "uuid_utils.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include <userver/engine/task/current_task.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/utils/assert.hpp>

namespace v1 {

struct [[nodiscard]] ParamError final {
    String name;
    String message;
};

enum class ClientRequestError {
    kInvalidClientIp,
    kCrudFailure,
};

class [[nodiscard]] HandlerRequestSupport final {
public:
    HandlerRequestSupport(Crud &crud, const Config &config) : crud(crud), config(config) {}

    void applyRequestDeadline(
        const server::http::HttpRequest &request, std::chrono::milliseconds requestTimeout
    ) const
    {
        auto finalDeadline = computeHandlerDeadline(request, requestTimeout);
        eng::current_task::SetDeadline(finalDeadline);
    }

    [[nodiscard]] Expected<String, ParamError>
    parseRequiredQueryText(const server::http::HttpRequest &request, String paramName) const
    {
        const std::string arg = request.GetArg(std::to_string(paramName));
        ENSURE(!arg.empty(), missingParamError(paramName));
        return TRY_MAP_ERR(String::fromBytes(arg), ([&](const auto &) {
                               return invalidParamError(paramName);
                           }));
    }

    [[nodiscard]] Expected<String, ParamError>
    parseQueryText(const server::http::HttpRequest &request, String paramName) const
    {
        const std::string arg = request.GetArg(std::to_string(paramName));
        return TRY_MAP_ERR(String::fromBytes(arg), ([&](const auto &) {
                               return invalidParamError(paramName);
                           }));
    }

    [[nodiscard]] Expected<Link, ParamError>
    parseRequiredQueryLink(const server::http::HttpRequest &request, String paramName) const
    {
        const auto text = TRY(parseRequiredQueryText(request, paramName));
        return TRY_MAP_ERR(Link::fromText(text, config.urlBytesMax()), ([&](const auto &) {
                               return invalidParamError(paramName);
                           }));
    }

    [[nodiscard]] Expected<uuidu::Uuid, ParamError>
    parseUuidPathArg(const server::http::HttpRequest &request, String paramName) const
    {
        const auto text = TRY(parseRequiredPathText(request, paramName));
        return TRY_OK_OR(uuidu::parse(text.view()), invalidParamError(paramName));
    }

    [[nodiscard]] Expected<std::optional<ClientIpCooldown>, ClientRequestError>
    checkClientIpCooldown(const server::http::HttpRequest &request) const
    {
        auto clientIp = client_ip::resolve(request, config);
        if (!clientIp)
            return Unex(ClientRequestError::kInvalidClientIp);

        auto cooldown = crud.acquireClientIpCooldown(std::move(*clientIp));
        if (!cooldown)
            return Unex(ClientRequestError::kCrudFailure);
        return *cooldown;
    }

private:
    [[nodiscard]] static ParamError missingParamError(String paramName)
    {
        using namespace text::literals;

        return {paramName, "missing parameter"_t};
    }

    [[nodiscard]] static ParamError invalidParamError(String paramName)
    {
        using namespace text::literals;

        return {paramName, "invalid parameter"_t};
    }

    [[nodiscard]] Expected<String, ParamError>
    parseRequiredPathText(const server::http::HttpRequest &request, String paramName) const
    {
        const std::string arg = request.GetPathArg(std::to_string(paramName));
        ENSURE(!arg.empty(), missingParamError(paramName));
        return TRY_MAP_ERR(String::fromBytes(arg), ([&](const auto &) {
                               return invalidParamError(paramName);
                           }));
    }

    Crud &crud;
    const Config &config;
};

[[nodiscard]] inline std::string
respondClientRequestError(server::http::HttpResponse &response, ClientRequestError error)
{
    using enum server::http::HttpStatus;
    using namespace text::literals;
    using enum ClientRequestError;

    switch (error) {
    case kInvalidClientIp:
        return httpu::respondError(response, kBadRequest, "invalid client ip"_t);
    case kCrudFailure:
        return httpu::respondError(response, kInternalServerError, "internal server error"_t);
    default:
        invariant(false, "");
    }
    return {};
}

} // namespace v1
