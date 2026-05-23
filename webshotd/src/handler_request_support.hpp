#pragma once

#include "config.hpp"
#include "http.hpp"
#include "json.hpp"
#include "link.hpp"
#include "schema/common/common.hpp"
#include "text.hpp"
#include "try.hpp"
#include "uuid_utils.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <userver/http/common_headers.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/utils/assert.hpp>

namespace ws {

namespace us = userver;
namespace server = us::server;

struct [[nodiscard]] ParamError final {
    String name;
    String message;
};

template <typename T>
[[nodiscard]] Expected<T, String> ParseJsonBodyRequest(const server::http::HttpRequest &request)
{
    using namespace text::literals;

    auto body = TRY_MAP_ERR(String::FromBytes(request.RequestBody()), [](const auto &) {
        return "invalid request body"_t;
    });
    return ws::json::Parse<T>(body, "invalid request body"_t);
}

[[nodiscard]] inline Expected<Link, String>
ParseJsonLinkBody(const server::http::HttpRequest &request, const Config &config)
{
    using namespace text::literals;

    auto body = TRY(ParseJsonBodyRequest<::dto::LinkRequest>(request));
    auto text = TRY_ERR_AS(String::FromBytes(body.link), "invalid parameter"_t);
    return TRY_ERR_AS(Link::FromText(text, config.UrlBytesMax()), "invalid parameter"_t);
}

class [[nodiscard]] HandlerRequestSupport final {
public:
    explicit HandlerRequestSupport(const Config &config) : config_(config) {}

    [[nodiscard]] Expected<String, ParamError>
    ParseRequiredQueryText(const server::http::HttpRequest &request, String param_name) const
    {
        const std::string param_value = request.GetArg(param_name.ToBytes());
        ENSURE(!param_value.empty(), MissingParamError(param_name));
        return TRY_MAP_ERR(String::FromBytes(param_value), ([&](auto) {
                               return InvalidParamError(param_name);
                           }));
    }

    [[nodiscard]] Expected<String, ParamError>
    ParseQueryText(const server::http::HttpRequest &request, String param_name) const
    {
        const std::string param_value = request.GetArg(param_name.ToBytes());
        return TRY_MAP_ERR(String::FromBytes(param_value), ([&](auto) {
                               return InvalidParamError(param_name);
                           }));
    }

    [[nodiscard]] Expected<Link, ParamError>
    ParseRequiredQueryLink(const server::http::HttpRequest &request, String param_name) const
    {
        auto text = TRY(ParseRequiredQueryText(request, param_name));
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
        auto text = TRY_MAP_ERR(String::FromBytes(bytes), ([&](auto) {
                                    return InvalidParamError(param_name);
                                }));
        return ParseLinkText(text, param_name);
    }

    [[nodiscard]] Expected<ws::uuid::Uuid, ParamError>
    ParseRequiredPathParamUuid(const server::http::HttpRequest &request, String param_name) const
    {
        auto text = TRY(ParseRequiredPathText(request, param_name));
        return TRY_OK_OR(ws::uuid::Parse(text.View()), InvalidParamError(param_name));
    }

    [[nodiscard]] std::optional<String> RequestHost(const server::http::HttpRequest &request) const
    {
        return RequestHeader(request, us::http::headers::kHost);
    }

    [[nodiscard]] std::optional<String>
    RequestForwardedHost(const server::http::HttpRequest &request) const
    {
        return RequestHeader(request, "X-Forwarded-Host");
    }

    [[nodiscard]] std::optional<String>
    RequestForwardedProto(const server::http::HttpRequest &request) const
    {
        return RequestHeader(request, "X-Forwarded-Proto");
    }

private:
    [[nodiscard]] static std::optional<String>
    RequestHeader(const server::http::HttpRequest &request, std::string_view name)
    {
        auto value = request.GetHeader(std::string{name});
        if (value.empty())
            return {};
        return TRY(String::FromBytes(value));
    }

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
        const std::string param_value = request.GetPathArg(param_name.ToBytes());
        ENSURE(!param_value.empty(), MissingParamError(param_name));
        return TRY_MAP_ERR(String::FromBytes(param_value), ([&](auto) {
                               return InvalidParamError(param_name);
                           }));
    }

    const Config &config_;
};

} // namespace ws
