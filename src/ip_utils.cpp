#include "include/ip_utils.hpp"
/**
 * @file
 * @brief Helpers for validating hostnames and public‑routable IP ranges.
 */

#include <algorithm>
#include <array>
#include <string>

#include <arpa/inet.h>

namespace IpUtils {

/** See header for semantics. */
bool isIpLiteralHostname(std::string_view hostname) noexcept
{
    if (hostname.empty())
        return false;
    // Bracketed IPv6 literal per RFC 3986
    if (hostname.front() == '[' && hostname.back() == ']') {
        const std::string inside(hostname.substr(1, hostname.size() - 2));
        in6_addr addr6;
        return inet_pton(AF_INET6, inside.c_str(), &addr6) == 1;
    }
    // Plain IPv4 dotted-decimal
    in_addr addr4;
    std::string hostStr(hostname);
    return inet_pton(AF_INET, hostStr.c_str(), &addr4) == 1;
}

/** Check if an IPv4 is within a CIDR range (arguments in host order). */
static inline bool inRange(uint32_t ip, uint32_t net, uint32_t mask)
{
    return (ip & mask) == (net & mask);
}

bool isPublicRoutableIPv4(uint32_t ipHostOrder) noexcept
{
    if (inRange(ipHostOrder, 0x00000000u, 0xFF000000u)) // 0.0.0.0/8
        return false;
    if (inRange(ipHostOrder, 0x0A000000u, 0xFF000000u)) // 10.0.0.0/8
        return false;
    if (inRange(ipHostOrder, 0x64400000u, 0xFFC00000u)) // 100.64.0.0/10
        return false;
    if (inRange(ipHostOrder, 0x7F000000u, 0xFF000000u)) // 127.0.0.0/8
        return false;
    if (inRange(ipHostOrder, 0xA9FE0000u, 0xFFFF0000u)) // 169.254.0.0/16
        return false;
    if (inRange(ipHostOrder, 0xAC100000u, 0xFFF00000u)) // 172.16.0.0/12
        return false;
    if (inRange(ipHostOrder, 0xC0A80000u, 0xFFFF0000u)) // 192.168.0.0/16
        return false;
    if (inRange(ipHostOrder, 0xC6120000u, 0xFFFE0000u)) // 198.18.0.0/15 (benchmarking/test net)
        return false;
    if (inRange(ipHostOrder, 0xE0000000u, 0xF0000000u)) // 224.0.0.0/4 (multicast)
        return false;
    if (inRange(ipHostOrder, 0xF0000000u, 0xF0000000u)) // 240.0.0.0/4 (reserved)
        return false;
    return true;
}

bool isPublicRoutableIPv6(const in6_addr &addr) noexcept
{
    const unsigned char *v = addr.s6_addr;

    // ::/128
    if (std::all_of(v, v + 16, [](unsigned char c) { return c == 0; }))
        return false;

    // ::1/128
    if (v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 0 && v[4] == 0 && v[5] == 0 && v[6] == 0 &&
        v[7] == 0 && v[8] == 0 && v[9] == 0 && v[10] == 0 && v[11] == 0 && v[12] == 0 &&
        v[13] == 0 && v[14] == 0 && v[15] == 1)
        return false;

    // fc00::/7 (Unique Local Address)
    if ((v[0] & 0xFEu) == 0xFCu)
        return false;

    // fe80::/10 (link-local)
    if (v[0] == 0xFEu && (v[1] & 0xC0u) == 0x80u)
        return false;

    // ::ffff:0:0/96 (IPv4-mapped IPv6 addresses)
    if (std::equal(
            v, v + 10, std::array<unsigned char, 10>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0}.begin()
        ) &&
        v[10] == 0xFFu && v[11] == 0xFFu)
        return false;

    // ff00::/8 (multicast)
    if (v[0] == 0xFFu)
        return false;

    return true;
}

} // namespace IpUtils
