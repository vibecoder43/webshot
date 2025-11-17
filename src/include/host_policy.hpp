#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include <userver/clients/dns/resolver.hpp>

namespace us = userver;

namespace v1::hostpolicy {

/** @return true if `host_lower` has no dots. */
bool IsBareName(const std::string &host_lower);
/** @return true for names explicitly blocked regardless of resolution. */
bool IsDeniedHostname(const std::string &host_lower);
/** @return true if the name ends with a reserved/special TLD like `.local`. */
bool HasSpecialTldSuffix(std::string_view host_lower);

/**
 * @brief Resolve a hostname and return public IPv4/IPv6 addresses.
 *
 * Performs DNS resolution and filters out private, link‑local, multicast and
 * other non‑public ranges.
 *
 * @param resolver userver DNS resolver.
 * @param host Hostname to resolve.
 * @param timeout Resolution timeout.
 * @return List of public addresses (string form). Empty on failure.
 */
std::vector<std::string> resolvePublic(
    us::clients::dns::Resolver &resolver, const std::string &host, std::chrono::milliseconds timeout
);

} // namespace v1::hostpolicy
