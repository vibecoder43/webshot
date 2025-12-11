#include "host_policy.hpp"
/**
 * @file
 * @brief Host policy checks and DNS resolution for public addresses.
 */
#include "ip_utils.hpp"
#include "text.hpp"

#include <array>
#include <exception>
#include <string>
#include <string_view>

#include <arpa/inet.h>

#include <userver/engine/io/sockaddr.hpp>

namespace us = userver;
namespace engine = us::engine;
using namespace text::literals;

namespace v1::HostPolicy {

bool isBareName(const String &host) { return host.view().find('.') == std::string_view::npos; }

bool isDeniedHostname(const String &host) { return host == "localhost"_t; }

bool hasSpecialTldSuffix(String host)
{
    static const std::array<String, 6> kTlds{".local"_t,   ".home.arpa"_t, ".test"_t,
                                             ".invalid"_t, ".example"_t,   "internal"_t};
    const auto hv = host.view();
    for (const auto &tldWithDot : kTlds) {
        const auto tld = tldWithDot.view();
        const auto plain = tld.substr(1);
        if (hv == plain)
            return true;
        const auto tldSize = tld.size();
        if (hv.size() >= tldSize && hv.compare(hv.size() - tldSize, tldSize, tld) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<String>
resolvePublic(us::clients::dns::Resolver &resolver, const String &host, engine::Deadline deadline)
{
    std::vector<String> ips;
    try {
        auto addrs = resolver.Resolve(std::string(host.view()), deadline);
        for (const auto &sa : addrs) {
            switch (sa.Domain()) {
            case userver::engine::io::AddrDomain::kInet: {
                const auto *sin = sa.As<struct sockaddr_in>();
                if (IpUtils::isPublicIpv4(sin->sin_addr))
                    ips.emplace_back(String::fromBytesThrow(sa.PrimaryAddressString()));
                break;
            }
            case userver::engine::io::AddrDomain::kInet6: {
                const auto *sin = sa.As<struct sockaddr_in6>();
                if (IpUtils::isPublicIpv6(sin->sin6_addr))
                    ips.emplace_back(String::fromBytesThrow(sa.PrimaryAddressString()));
                break;
            }
            default:
                break;
            }
            if (ips.size() >= 32)
                break;
        }
    } catch (std::exception &) {
        // return empty to signal failure
    }
    return ips;
}

} // namespace v1::HostPolicy
