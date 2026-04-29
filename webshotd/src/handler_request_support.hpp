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
#include "userver_namespaces.hpp"
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

namespace v1 {

using text::toBytes;

struct [[nodiscard]] ParamError final {
    String name;
    String message;
};

template <typename T>
[[nodiscard]] Expected<T, String> parseJsonBodyRequest(const server::http::HttpRequest &request)
{
    using namespace text::literals;

    const auto body = TRY_MAP_ERR(String::fromBytes(request.RequestBody()), [](const auto &) {
        return "invalid request body"_t;
    });
    return exu::json::parse<T>(body, "invalid request body"_t);
}

[[nodiscard]] inline Expected<Link, String>
parseJsonLinkBody(const server::http::HttpRequest &request, const Config &config)
{
    using namespace text::literals;

    const auto body = TRY(parseJsonBodyRequest<::dto::LinkRequest>(request));
    const auto text = TRY_ERR_AS(String::fromBytes(body.link), "invalid parameter"_t);
    return TRY_ERR_AS(Link::fromText(text, config.urlBytesMax()), "invalid parameter"_t);
}

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
        const std::string arg = request.GetArg(toBytes(paramName));
        ENSURE(!arg.empty(), missingParamError(paramName));
        return TRY_MAP_ERR(String::fromBytes(arg), ([&](const auto &) {
                               return invalidParamError(paramName);
                           }));
    }

    [[nodiscard]] Expected<String, ParamError>
    parseQueryText(const server::http::HttpRequest &request, String paramName) const
    {
        const std::string arg = request.GetArg(toBytes(paramName));
        return TRY_MAP_ERR(String::fromBytes(arg), ([&](const auto &) {
                               return invalidParamError(paramName);
                           }));
    }

    [[nodiscard]] Expected<Link, ParamError>
    parseRequiredQueryLink(const server::http::HttpRequest &request, String paramName) const
    {
        const auto text = TRY(parseRequiredQueryText(request, paramName));
        return parseLinkText(text, paramName);
    }

    template <typename T>
    [[nodiscard]] Expected<T, String> parseJsonBody(const server::http::HttpRequest &request) const
    {
        return parseJsonBodyRequest<T>(request);
    }

    [[nodiscard]] Expected<Link, ParamError>
    parseLinkText(const String &text, String paramName) const
    {
        return TRY_MAP_ERR(Link::fromText(text, config.urlBytesMax()), ([&](const auto &) {
                               return invalidParamError(paramName);
                           }));
    }

    [[nodiscard]] Expected<Link, ParamError>
    parseLinkBytes(std::string_view bytes, String paramName) const
    {
        const auto text = TRY_MAP_ERR(String::fromBytes(bytes), ([&](const auto &) {
                                          return invalidParamError(paramName);
                                      }));
        return parseLinkText(text, paramName);
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
        auto clientIp = client::ip::resolve(request, config);
        if (!clientIp)
            return Unex(ClientRequestError::kInvalidClientIp);

        auto cooldown = crud.acquireClientIpCooldown(std::move(*clientIp));
        if (!cooldown)
            return Unex(ClientRequestError::kCrudFailure);
        return *cooldown;
    }

    [[nodiscard]] std::optional<String> requestHost(const server::http::HttpRequest &request) const
    {
        auto host = String::fromBytes(request.GetHeader(us::http::headers::kHost));
        if (!host)
            return {};
        return *host;
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
        const std::string arg = request.GetPathArg(toBytes(paramName));
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
        invariant(""_t);
    }
}

} // namespace v1
