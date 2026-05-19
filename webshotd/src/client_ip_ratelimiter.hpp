#pragma once
/**
 * @file
 * @brief In-process per-client-IP rate limiter for HTTP handlers.
 */

#include "ip.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include <userver/cache/lru_cache_component_base.hpp>
#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/yaml_config/schema.hpp>

namespace ws {

namespace us = userver;

struct [[nodiscard]] ClientIpRatelimit final {
    std::chrono::milliseconds retry_after;
};

class [[nodiscard]] ClientIpRatelimiter final
    : public us::cache::LruCacheComponent<
          Ip, std::shared_ptr<struct IpLimiter>, IpHash, std::equal_to<Ip>> {
public:
    static constexpr std::string_view kName = "client_ip_ratelimiter";

    ClientIpRatelimiter(
        const us::components::ComponentConfig &config,
        const us::components::ComponentContext &context
    );

    ~ClientIpRatelimiter() override;

    [[nodiscard]] static us::yaml_config::Schema GetStaticConfigSchema();

    [[nodiscard]] std::optional<ClientIpRatelimit> Acquire(const Ip &client_ip) noexcept;

private:
    using Base = us::cache::LruCacheComponent<
        Ip, std::shared_ptr<struct IpLimiter>, IpHash, std::equal_to<Ip>>;

    [[nodiscard]] std::shared_ptr<struct IpLimiter> DoGetByKey(const Ip &client_ip) override;

    const std::chrono::milliseconds interval;
};

} // namespace ws
