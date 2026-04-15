#pragma once

#include "config.hpp"
#include "ip_utils.hpp"
#include "text.hpp"
#include "userver_namespaces.hpp"

#include <optional>
#include <string>
#include <string_view>

#include <absl/strings/strip.h>
#include <userver/server/http/http_request.hpp>
#include <userver/utils/assert.hpp>

namespace v1::client_ip {
[[nodiscard]] inline std::optional<String> makeClientIp(std::string_view raw)
{
    std::string text{raw};
    absl::StripAsciiWhitespace(&text);
    if (!isIpLiteralHostname(text))
        return {};
    auto clientIp = String::fromBytes(text);
    if (!clientIp)
        return {};
    return clientIp.value();
}

[[nodiscard]] inline std::optional<String>
resolve(const server::http::HttpRequest &request, const Config &config)
{
    switch (config.clientIpSource()) {
    case ClientIpSource::kPeer:
        return makeClientIp(request.GetRemoteAddress().PrimaryAddressString());
    case ClientIpSource::kTrustedHeader:
        return makeClientIp(request.GetHeader(config.clientIpHeaderName()));
    default:
        UINVARIANT(false, "unknown client IP source");
        return {};
    }
}
} // namespace v1::client_ip
