#include "client_ip_ratelimiter.hpp"
/**
 * @file
 * @brief In-process per-client-IP rate limiter.
 */

#include "integers.hpp"
#include "invariant.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include <userver/cache/lru_map.hpp>
#include <userver/concurrent/variable.hpp>
#include <userver/engine/mutex.hpp>
#include <userver/utils/datetime.hpp>
#include <userver/utils/mock_now.hpp>
#include <userver/utils/token_bucket.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace ws {

namespace us = userver;
namespace concurrent = us::concurrent;
namespace datetime = us::utils::datetime;
namespace chrono = std::chrono;

using namespace std::chrono_literals;
using namespace text::literals;

namespace {

using us::utils::TokenBucket;

struct [[nodiscard]] IpLimiter final {
    TokenBucket bucket;
    TokenBucket::TimePoint last_allow;
};

} // namespace

class [[nodiscard]] ClientIpRatelimiter::Impl final {
public:
    explicit Impl(const us::components::ComponentConfig &cfg)
        : interval_(cfg["interval_ms"].As<int64_t>() * 1ms),
          cache_max_size_(cfg["cache_max_size"].As<size_t>()), limiters_(cache_max_size_)
    {
        Invariant(interval_ >= 0ms, "interval_ms must be >= 0"_t);
        Invariant(cache_max_size_ > 0, "cache_max_size must be > 0"_t);
    }

    std::optional<ClientIpRatelimit> Acquire(const Ip &client_ip) noexcept
    {
        if (interval_ == 0ms)
            return {};

        TokenBucket::Duration interval{chrono::duration_cast<TokenBucket::Duration>(interval_)};
        const auto now_tp = datetime::MockSteadyNow();

        auto lock = limiters_.UniqueLock();
        auto *limiter = lock->Emplace(
            client_ip, IpLimiter{
                           .bucket = TokenBucket(
                               1, TokenBucket::RefillPolicy{.amount = 1, .interval = interval}
                           ),
                           .last_allow = now_tp,
                       }
        );
        Invariant(limiter != nullptr, "failed to allocate per-IP ratelimit entry"_t);

        if (limiter->bucket.Obtain()) {
            limiter->last_allow = now_tp;
            return {};
        }

        chrono::milliseconds elapsed{
            chrono::duration_cast<chrono::milliseconds>(now_tp - limiter->last_allow)
        };
        chrono::milliseconds retry_after{0ms};
        if (elapsed < interval_)
            retry_after = interval_ - elapsed;

        return ClientIpRatelimit{.retry_after = retry_after};
    }

private:
    const chrono::milliseconds interval_;
    const size_t cache_max_size_;
    concurrent::Variable<
        us::cache::LruMap<Ip, IpLimiter, IpHash, std::equal_to<Ip>>, us::engine::Mutex>
        limiters_;
};

ClientIpRatelimiter::ClientIpRatelimiter(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context), impl_(std::make_unique<Impl>(config))
{
}

ClientIpRatelimiter::~ClientIpRatelimiter() = default;

us::yaml_config::Schema ClientIpRatelimiter::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<us::components::ComponentBase>(R"(
type: object
description: In-process per-client-IP rate limiter
additionalProperties: false
properties:
  interval_ms:
    type: integer
    minimum: 0
    description: Per-client-IP minimum interval between HTTP CRUD operations in milliseconds; 0 disables ratelimit
  cache_max_size:
    type: integer
    minimum: 1
    description: Max number of per-client-IP ratelimit entries kept in memory (LRU)
)");
}

std::optional<ClientIpRatelimit> ClientIpRatelimiter::Acquire(const Ip &client_ip) noexcept
{
    return impl_->Acquire(client_ip);
}

} // namespace ws
