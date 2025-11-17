#pragma once

#include <string_view>

#include <netinet/in.h>

namespace IpUtils {

/**
 * @brief Whether the input is an IP literal hostname.
 *
 * Supported forms:
 *  - Bracketed IPv6: "[2001:db8::1]"
 *  - IPv4 dotted‑decimal: "203.0.113.5"
 */
bool isIpLiteralHostname(std::string_view hostname) noexcept;

/** @brief IPv4 address (host byte order) passes public‑routable allowlist. */
bool isPublicRoutableIPv4(uint32_t ipHostOrder) noexcept;

/** @brief IPv6 address passes public‑routable allowlist rules. */
bool isPublicRoutableIPv6(const in6_addr &addr) noexcept;

} // namespace IpUtils
