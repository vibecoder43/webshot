#pragma once

#include <string>
#include <string_view>

#include <arpa/inet.h>

namespace v1 {

[[nodiscard]] inline bool isIpv4Address(std::string_view host) noexcept
{
    if (host.empty())
        return false;

    in_addr addr4{};
    const auto hostText = std::string(host);
    return inet_pton(AF_INET, hostText.c_str(), &addr4) == 1;
}

[[nodiscard]] inline bool isIpv6Address(std::string_view host) noexcept
{
    if (host.empty())
        return false;

    auto candidate = host;
    if (candidate.front() == '[' && candidate.back() == ']')
        candidate = candidate.substr(1, candidate.size() - 2);

    in6_addr addr6{};
    const auto hostText = std::string(candidate);
    return inet_pton(AF_INET6, hostText.c_str(), &addr6) == 1;
}

[[nodiscard]] inline bool isIpLiteralHostname(std::string_view host) noexcept
{
    return isIpv4Address(host) || isIpv6Address(host);
}

} // namespace v1
