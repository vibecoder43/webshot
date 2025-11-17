#include "include/host_policy.hpp"
/**
 * @file
 * @brief Host policy checks and DNS resolution for public addresses.
 */
#include "include/ip_utils.hpp"

#include <array>
#include <exception>
#include <string>
#include <string_view>

using namespace std::chrono_literals;
namespace us = userver;
namespace engine = us::engine;
namespace v1::hostpolicy {

bool IsBareName(const std::string &host_lower) { return host_lower.find('.') == std::string::npos; }

bool IsDeniedHostname(const std::string &host_lower)
{
    return (host_lower == "localhost" || host_lower == "host.docker.internal");
}

bool HasSpecialTldSuffix(std::string_view host_lower)
{
    static const std::array<std::string_view, 5> kTlds{
        ".local", ".home.arpa", ".test", ".invalid", ".example"
    };

    for (const auto tldWithDot : kTlds) {
        const auto tldSize = tldWithDot.size();
        if (host_lower.size() >= tldSize) {
            if (host_lower.compare(host_lower.size() - tldSize, tldSize, tldWithDot) == 0)
                return true;
        }

        const std::string_view plainTld = tldWithDot.substr(1);
        if (host_lower == plainTld)
            return true; // plain tld (e.g., "local") - unlikely after bare-name check
    }
    return false;
}

static bool IsPublicV4(uint32_t ip_be)
{
    const uint32_t ip_host = ::ntohl(ip_be);
    return IpUtils::isPublicRoutableIPv4(ip_host);
}

static bool IsPublicV6(const struct in6_addr &a) { return IpUtils::isPublicRoutableIPv6(a); }

std::vector<std::string> resolvePublic(
    us::clients::dns::Resolver &resolver, const std::string &host, std::chrono::milliseconds timeout
)
{
    std::vector<std::string> out;
    try {
        auto addrs = resolver.Resolve(host, engine::Deadline::FromDuration(timeout));
        for (const auto &sa : addrs) {
            switch (sa.Domain()) {
            case userver::engine::io::AddrDomain::kInet: {
                const auto *sin = sa.As<struct sockaddr_in>();
                if (IsPublicV4(sin->sin_addr.s_addr))
                    out.emplace_back(sa.PrimaryAddressString());
                break;
            }
            case userver::engine::io::AddrDomain::kInet6: {
                const auto *sin6 = sa.As<struct sockaddr_in6>();
                if (IsPublicV6(sin6->sin6_addr))
                    out.emplace_back(sa.PrimaryAddressString());
                break;
            }
            default:
                break;
            }
            if (out.size() >= 5)
                break;
        }
    } catch (std::exception &) {
        // swallow, return empty to signal failure
    }
    return out;
}

} // namespace v1::hostpolicy
