#include "denylist.hpp"
/**
 * @file
 * @brief Host denylist checks and persistence backed by Postgres.
 */
#include <webshot/sql_queries.hpp>

#include "database.hpp"
#include "invariant.hpp"
#include "prefix_utils.hpp"
#include "text_postgres_formatter.hpp"
#include "try.hpp"
#include "userver_namespaces.hpp"

#include <format>
#include <string>
#include <utility>

#include <userver/components/component.hpp>
#include <userver/logging/log.hpp>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/component.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/yaml_config.hpp>
namespace pg = us::storages::postgres;
namespace sql = webshot::sql;

namespace v1 {
using namespace text::literals;

String accessDecisionMessage(AccessDecisionReason reason)
{
    using enum AccessDecisionReason;

    switch (reason) {
    case kAllowed:
        return "allowed"_t;
    case kDenylisted:
        return "host in denylist"_t;
    case kNotAllowlisted:
        return "link not in allowlist"_t;
    case kNonHttps:
        return "non-HTTPS fetch blocked"_t;
    default:
        invariant(""_t);
    }
}

us::yaml_config::Schema Denylist::GetStaticConfigSchema()
{
    return us::yaml_config::MergeSchemas<us::components::ComponentBase>(R"(
type: object
description: 'denylist component'
additionalProperties: false
properties: {}
)");
}

struct Denylist::Impl {
    explicit Impl(
        const us::components::ComponentConfig &, const us::components::ComponentContext &context
    )
        : cluster(context.FindComponent<us::components::Postgres>("shared_state_db").GetCluster())
    {
    }

    template <pg::ClusterHostType Host, typename F, typename... Ts>
    [[nodiscard]] auto execDb(F &&f, Ts &&...args) const
    {
        return pgx::execute<Host>(cluster, std::forward<F>(f), std::forward<Ts>(args)...);
    }

    template <typename F, typename... Ts> [[nodiscard]] auto readonly(F &&f, Ts &&...args) const
    {
        return execDb<pg::ClusterHostType::kSlaveOrMaster>(
            std::forward<F>(f), std::forward<Ts>(args)...
        );
    }

    template <typename F, typename... Ts> [[nodiscard]] auto readwrite(F &&f, Ts &&...args) const
    {
        return execDb<pg::ClusterHostType::kMaster>(std::forward<F>(f), std::forward<Ts>(args)...);
    }

    pg::ClusterPtr cluster;
};

Denylist::Denylist(
    const us::components::ComponentConfig &config, const us::components::ComponentContext &context
)
    : us::components::ComponentBase(config, context), impl(std::make_unique<Impl>(config, context))
{
}

Denylist::~Denylist() = default;

Expected<bool, DenylistError> Denylist::isAllowedPrefix(const String &prefixKey)
{
    return !TRY(isDeniedPrefix(prefixKey));
}

Expected<bool, DenylistError> Denylist::isDeniedPrefix(const String &prefixKey)
{
    const auto tree = prefix::makePrefixTree(prefixKey);
    return TRY_MAP_ERR(
        impl->readonly(
            [&](auto &res) { return res.template AsSingleRow<bool>(); }, sql::kCheckDenylistTree,
            tree
        ),
        [](const auto &error) {
            LOG_ERROR() << std::format("denylist check failed: {}", error.what);
            return DenylistError::kDbFailure;
        }
    );
}

Expected<bool, DenylistError> Denylist::isAllowlistedPrefix(const String &prefixKey)
{
    const auto tree = prefix::makePrefixTree(prefixKey);
    return TRY_MAP_ERR(
        impl->readonly(
            [&](auto &res) { return res.template AsSingleRow<bool>(); }, sql::kCheckAllowlistTree,
            tree
        ),
        [](const auto &error) {
            LOG_ERROR() << std::format("allowlist check failed: {}", error.what);
            return DenylistError::kDbFailure;
        }
    );
}

Expected<AccessDecision, DenylistError>
Denylist::evaluatePrefix(const String &prefixKey, AccessPolicyMode mode)
{
    using enum AccessDecisionReason;
    using enum AccessPolicyMode;

    if (mode == kRegular) {
        const auto allowlisted = TRY(isAllowlistedPrefix(prefixKey));
        if (allowlisted)
            return AccessDecision{.allowed = true, .reason = kAllowed};

        const auto denied = TRY(isDeniedPrefix(prefixKey));
        if (denied)
            return AccessDecision{.allowed = false, .reason = kDenylisted};
        return AccessDecision{.allowed = true, .reason = kAllowed};
    }

    const auto denied = TRY(isDeniedPrefix(prefixKey));
    if (denied)
        return AccessDecision{.allowed = false, .reason = kDenylisted};

    const auto allowlisted = TRY(isAllowlistedPrefix(prefixKey));
    if (!allowlisted)
        return AccessDecision{.allowed = false, .reason = kNotAllowlisted};
    return AccessDecision{.allowed = true, .reason = kAllowed};
}

Expected<void, DenylistError> Denylist::insertPrefix(const String &prefixKey, const String &reason)
{
    const auto tree = prefix::makePrefixTree(prefixKey);
    const auto inserted = impl->readwrite(
        [&](auto &res) { static_cast<void>(res); }, sql::kInsertDenylistHost, prefixKey, tree,
        reason
    );
    if (!inserted) {
        LOG_CRITICAL() << std::format(
            "denylist insert failed for {}: {}", prefixKey, inserted.error().what
        );
        us::utils::AbortWithStacktrace("denylist insert failed");
    }
    return {};
}

Expected<void, DenylistError>
Denylist::insertAllowlistPrefix(const String &prefixKey, const String &reason)
{
    const auto tree = prefix::makePrefixTree(prefixKey);
    const auto inserted = impl->readwrite(
        [&](auto &res) { static_cast<void>(res); }, sql::kInsertAllowlistHost, prefixKey, tree,
        reason
    );
    if (!inserted) {
        LOG_ERROR() << std::format(
            "allowlist insert failed for {}: {}", prefixKey, inserted.error().what
        );
        return Unex(DenylistError::kDbFailure);
    }
    return {};
}

Expected<void, DenylistError> Denylist::removeAllowlistPrefix(const String &prefixKey)
{
    const auto removed = impl->readwrite(
        [&](auto &res) { static_cast<void>(res); }, sql::kDeleteAllowlistHost, prefixKey
    );
    if (!removed) {
        LOG_ERROR() << std::format(
            "allowlist delete failed for {}: {}", prefixKey, removed.error().what
        );
        return Unex(DenylistError::kDbFailure);
    }
    return {};
}

} // namespace v1
