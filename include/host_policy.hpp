#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <userver/clients/dns/resolver.hpp>
#include <userver/engine/deadline.hpp>

namespace us = userver;

namespace v1::HostPolicy {

/** @return true if `host` has no dots. */
bool IsBareName(const std::string &host);
/** @return true for names explicitly blocked regardless of resolution. */
bool IsDeniedHostname(const std::string &host);
/** @return true if the name ends with a reserved/special TLD like `.local`. */
bool HasSpecialTldSuffix(std::string_view host);

/**
 * @brief Resolve a hostname and return public IPv4/IPv6 addresses.
 *
 * Performs DNS resolution and filters out private, link‑local, multicast and
 * other non‑public ranges.
 *
 * @param resolver userver DNS resolver.
 * @param host Host to resolve.
 * @param deadline Resolution deadline.
 * @return List of public addresses (string form). Empty on failure.
 */
std::vector<std::string> resolvePublic(
    us::clients::dns::Resolver &resolver, const std::string &host, us::engine::Deadline deadline
);

} // namespace v1::HostPolicy
